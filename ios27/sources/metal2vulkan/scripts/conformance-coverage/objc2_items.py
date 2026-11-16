#!/usr/bin/env python3
"""Read the Metal selectors `objc2-metal` binds, for `objc2_metal.tsv`.

`objc2-metal`'s generated sources carry each binding's selector in an attribute beside the Rust
method it generates:

    #[unsafe(method(setSamplePositions:count:))]
    fn setSamplePositions_count(&self, positions: NonNull<MTLSamplePosition>, count: NSUInteger);

So the crate itself says which Metal call a Rust name makes, which type declares it, and what the
call answers. That is what resolves a call site in the suite's Metal leg to an item of the SDK: the
Rust name alone is ambiguous (twenty types bind `setLabel:`) and a receiver's type is only known by
following what made it.

This runs where the crate's sources are, which is a Mac that has fetched them; `coverage.py --api`
is the check that the committed table still says what they say.
"""

import re
import sys
from pathlib import Path

#: A Rust return type, reduced to the Objective-C type it answers. `Option`, `Retained`,
#: `ProtocolObject<dyn _>` and references are wrappers over the type that matters for following a
#: receiver; everything else (a scalar, an enum, a collection) answers no Metal object and is left
#: as the empty string.
WRAPPERS = re.compile(r"\b(?:Option|Retained|ProtocolObject|NonNull|Box|dyn)\b")


def answered(ret: str, owner: str = "") -> str:
    """The Metal class or protocol `ret` answers, or `""` where it answers no Metal object.

    A return of `Self` — which is how `objc2` spells a class's own `new` and `alloc` — answers the
    type that declares it, so following a descriptor's constructor lands on the descriptor.
    """
    if owner and re.search(r"\bSelf\b", ret):
        return owner
    bare = WRAPPERS.sub("", ret).replace("&", "").replace("<", " ").replace(">", " ")
    names = [word for word in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", bare) if word.startswith("MTL")]
    # A generic over one Metal type answers that type; anything else (`NSArray<MTLDevice>`, a
    # tuple, a scalar) is not a receiver this follows.
    return names[0] if len(names) == 1 else ""


def block_at(text: str, open_at: int) -> str:
    """The brace block that opens at `open_at`."""
    depth = 0
    for i, c in enumerate(text[open_at:]):
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_at : open_at + i + 1]
    return text[open_at:]


def methods(body: str, owner: str = ""):
    """Every `(selector, rust name, answered type)` the declarations in `body` bind."""
    for match in re.finditer(r"#\[unsafe\(method\(([^)]*)\)\)\]", body):
        rest = body[match.end() :]
        # `r#type`, which is how `objc2` spells a binding whose selector is a Rust keyword. Without
        # the `r#` the search steps over such a method and lands on the **next** one, which keyed
        # ten selectors to a neighbour's Rust name: `MTLHeap`'s `type` read as
        # `newBufferWithLength_options_offset`, and `MTLArgument`'s and `MTLBinding`'s as `access`.
        # So a call to the neighbour was credited to `type`, and the real getter to nothing.
        fn = re.search(
            r"\n\s*(?:pub\s+)?(?:unsafe\s+)?fn\s+(?:r#)?([A-Za-z_][A-Za-z0-9_]*)\s*\(", rest
        )
        if not fn:
            continue
        # The signature ends at the `;` or `{` that follows its parameter list, so a return type
        # spanning lines is read whole.
        after = rest[fn.end() :]
        end = min((i for i in (after.find(";"), after.find("{")) if i >= 0), default=len(after))
        ret = after[:end]
        arrow = ret.rfind("->")
        answers = answered(ret[arrow + 2 :], owner) if arrow >= 0 else ""
        yield match.group(1).strip(), fn.group(1), answers


def items(src: Path):
    """Every `(kind, type, supertypes, selector, rust name, answers)` the sources bind."""
    for path in sorted(src.rglob("*.rs")):
        text = path.read_text()
        # A protocol's methods are a trait's; its supertraits are the protocols it extends, and a
        # receiver of this type can be sent any of their selectors.
        for match in re.finditer(
            r"pub unsafe trait ([A-Za-z_][A-Za-z0-9_]*)\s*(?::([^{]*))?\{", text
        ):
            supers = [
                word
                for word in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", match.group(2) or "")
                if word.startswith("MTL") or word.startswith("NS")
            ]
            body = block_at(text, match.end() - 1)
            for selector, name, answers in methods(body, match.group(1)):
                yield "protocol", match.group(1), supers, selector, name, answers
        # A class's methods are an inherent `impl`'s, and `#[unsafe(super(_))]` names the class it
        # inherits from.
        for match in re.finditer(r"#\[unsafe\(super\(([^)]*)\)\)\][^;{]*?pub struct (\w+)", text):
            parents = [
                word.strip()
                for word in match.group(1).split(",")
                if word.strip().startswith("MTL") or word.strip().startswith("NS")
            ]
            yield "class", match.group(2), parents, "", "", ""
        for match in re.finditer(r"\nimpl ([A-Za-z_][A-Za-z0-9_]*)\s*\{", text):
            body = block_at(text, match.end() - 1)
            for selector, name, answers in methods(body, match.group(1)):
                yield "class", match.group(1), [], selector, name, answers


def rows(src: Path) -> list[str]:
    """The table's rows, deduplicated and sorted."""
    supers: dict[str, list[str]] = {}
    kinds: dict[str, str] = {}
    out = set()
    for kind, ty, parents, selector, name, answers in items(src):
        kinds[ty] = kind
        if parents:
            supers.setdefault(ty, []).extend(p for p in parents if p not in supers.get(ty, []))
        if selector:
            out.add((ty, selector, name, answers))
    lines = [f"{ty}\t{kinds[ty]}\t{' '.join(supers.get(ty, []))}" for ty in sorted(kinds)]
    calls = [f"{ty}\t{selector}\t{name}\t{answers}" for ty, selector, name, answers in sorted(out)]
    return lines, calls


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    types, calls = rows(Path(sys.argv[1]))
    print("# type\tkind\tsupertypes")
    for line in types:
        print(line)
    print("# type\tselector\trust\tanswers")
    for line in calls:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
