# Recordings

One file per case: Metal's answer, keyed by chip, macOS build and the macOS release whose Metal it
is (`metal_level`, the release the runtime answers as), with the runs behind it and the set of
outcomes where the behaviour varies.

A recording of another chip, macOS build, file format or suite version is stale. The suite refuses
it rather than comparing against it; nothing here ever passes quietly.

Each answer holds **two** Metal facts. `outcomes` is plain Metal's, the behavioural oracle, and is
what comparison uses. `validated` is what the same case did under Metal's own API validation layer,
taken in a child process under `MTL_DEBUG_LAYER=1` and `MTL_DEBUG_LAYER_ERROR_MODE=nslog`:

- `stopped` — Metal's validation refused a call, with the call it belongs to and the words it used,
  stripped of this host's `NSLog` prefix. Under Metal's default error mode the same complaint ends
  the process, which is why the suite does not use that mode.
- `ran` — the layer complained about nothing.
- `not_run` — the leg was never taken, so there is no such fact. A case whose answer is carried
  holds this. It is recorded, never guessed at: "the validation let this through" and "nobody asked
  the validation" are different facts and the report keeps them apart.

`validated` is never compared with our behaviour, and it never makes a case compliant: Metal's API
validation stopping a call is not Metal refusing it. It may document a refusal of ours, which keeps
the better bug report in the record, and it is held to the words the case names.

These are the conformance suite's own files in its own format. They are not the validation crate's
observations and they enter no census.

A **carried** answer is a Metal measurement this tree already holds, restated by the case in the
shape it emits, for a case Metal has no public call to answer: a texture fill, a two-plane
extended-range format, an indirect command's read-back. Its provenance names where it came from,
and where it was measured on another chip than the key's it says so there in words.
