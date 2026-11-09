// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../MarbleButtonReports.h"
#include <assert.h>
#include <vector>
#include <utility>
int main() {
    MarbleButtonReports queue;
    MarbleButtonReport nodes[9] = {};
    const uint32_t masks[] = {0,2,0,4,12,8,0,2,0};
    const uint64_t ticks[] = {1,100,108,120,130,140,150,200,2200};
    for (unsigned i=0;i<9;++i) {
        nodes[i].mask=masks[i];nodes[i].timestamp=ticks[i];
        assert(queue.append(&nodes[i]));
        // Each failed HID attempt keeps the original report, including its time.
        assert(queue.head==&nodes[0] && queue.head->timestamp==1);
    }
    assert(queue.count==9);
    std::vector<std::pair<uint32_t,uint64_t>> delivered;
    while(queue.head) {
        delivered.emplace_back(queue.head->mask,queue.head->timestamp);
        queue.completed();
    }
    assert(!queue.tail && queue.count==0 && queue.delivered==0);
    for(unsigned i=0;i<9;++i)assert(delivered[i]==std::make_pair(masks[i],ticks[i]));
    MarbleButtonReport duplicate{};duplicate.mask=0;duplicate.timestamp=5000;
    assert(!queue.append(&duplicate));
    MarbleButtonReport down{},up{};down.mask=2;down.timestamp=6000;up.mask=0;up.timestamp=6010;
    assert(queue.append(&down));
    // A shutdown release joins the queue while the press is waiting for retry.
    assert(queue.append(&up));assert(queue.head==&down);
    assert(queue.completed()==&down && queue.head==&up);
    assert(queue.completed()==&up && queue.delivered==0);
    return 0;
}
