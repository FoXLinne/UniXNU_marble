void ath11k_regd_update_chan_list_work(struct work_struct *work)
{
	struct ath11k *ar = container_of(to_delayed_work(work), struct ath11k,
					 channel_update_work);
	struct scan_chan_list_params *params;
	struct list_head local_update_list;

	INIT_LIST_HEAD(&local_update_list);
	spin_lock_bh(&ar->data_lock);
	list_splice_tail_init(&ar->channel_update_queue, &local_update_list);
	spin_unlock_bh(&ar->data_lock);

	while ((params = list_first_entry_or_null(&local_update_list,
						  struct scan_chan_list_params, list))) {
		/* The cooperative worker also dispatches the host scan that starts
		 * 11d discovery. Yield this job until that scan can make progress. */
		if (ar->state_11d != ATH11K_11D_IDLE) {
			if (!ar->channel_update_11d_deadline)
				ar->channel_update_11d_deadline = jiffies + ATH11K_SCAN_TIMEOUT_HZ;
			if (time_before(jiffies, ar->channel_update_11d_deadline))
				goto defer;
			ar->state_11d = ATH11K_11D_IDLE;
			complete(&ar->completed_11d_scan);
		}
		ar->channel_update_11d_deadline = 0;

		if (ar->scan.state == ATH11K_SCAN_STARTING ||
		    ar->scan.state == ATH11K_SCAN_RUNNING) {
			if (!ar->channel_update_scan_deadline)
				ar->channel_update_scan_deadline = jiffies + ATH11K_SCAN_TIMEOUT_HZ;
			if (time_before(jiffies, ar->channel_update_scan_deadline))
				goto defer;
		}
		ar->channel_update_scan_deadline = 0;
		ath11k_wmi_send_scan_chan_list_cmd(ar, params);
		list_del(&params->list);
		kfree(params);
		continue;

defer:
		/* Retain FIFO order ahead of updates queued during this callback. */
		spin_lock_bh(&ar->data_lock);
		list_splice_init(&local_update_list, &ar->channel_update_queue);
		spin_unlock_bh(&ar->data_lock);
		mod_delayed_work(ar->ab->workqueue, &ar->channel_update_work,
				 msecs_to_jiffies(20));
		return;
	}
	ar->channel_update_11d_deadline = 0;
	ar->channel_update_scan_deadline = 0;
}
