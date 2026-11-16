"""Resolve the Metal calls the conformance suite's native leg makes, to items of the SDK.

The suite's Metal leg reaches Metal through `objc2-metal`, whose Rust name for a binding is its
selector with each `:` turned into `_`. So a call site names its selector outright, and the one
thing a reader cannot see from the name is **which type** receives it: twenty types bind
`setLabel:`, four bind `setUsage:`, and the SDK item a call exercises is a property of the receiver.

This types the receiver. Within a function it follows what each local was made from — a class
method of a named Metal class, a getter whose answer `objc2_metal.tsv` records, a field of the
side's own struct, a parameter's declared type — and resolves the call against the type it lands
on, walking that type's supertypes to the one that declares the selector (`MTLTexture`'s `label`
is `MTLResource`'s item).

The resolution checks itself: a receiver typed `T` sent a selector that `objc2-metal` does not bind
on `T` or any of its supertypes is a **typing error**, not a Metal call, and
[`Leg.mistyped`][Leg] carries it so the caller can fail rather than quietly count the wrong item.
The honest limit is the other direction, and it is kept in two piles rather than one, because they
cost different things. A site whose selector `objc2-metal` binds on no Metal type at all
(`alloc`, `class`) is in [`Leg.foreign`][Leg]: no item exists, so nothing is hidden. A site whose
selector some Metal type DOES bind, but whose receiver this could not type, is in
[`Leg.unresolved`][Leg], and that one hides items: the call is real, one of the types binding the
name received it, and the caller can enumerate which. Pooled together they read as one harmless
limit, which is how eight items of one family read as uncalled while the leg demonstrably called
them. Neither pile is dropped.

A message to a class — `MTLRenderPassDescriptor::renderPassDescriptor()` — needs no typing at all:
the path names its receiver. It is resolved against that class the same way an instance send is
resolved against the type its receiver typed to.

A call site also carries where it sits: the function that holds it, and the **gates** around it, so
a call made only for one step of the case vocabulary is attributed to that step and not to every
case that encodes any step at all. Two kinds of gate are read, and a call carries every one that
encloses it:

* a `match` arm's variant, as `Enum::Variant` ([`arms_of`]), which a case satisfies by naming that
  variant;
* a guard on one field of the suite's **own** vocabulary, as `field:name` ([`guards_of`]), which a
  case satisfies by stating that field. One shared encoder sets fifty fields of a descriptor, each
  behind `if let Some(x) = pass.field`, so without this a case that states one of them reads as
  reaching all fifty.
"""

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

#: Rust types that wrap the Metal type a value really is.
UNWRAP = re.compile(r"\b(?:Option|Retained|ProtocolObject|Vec|Box|NonNull|dyn|mut|Self)\b")

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

#: Constructors that answer what they are given, so an expression wrapped in one types as its
#: content: the leg's helpers answer `Ok(heap.clone())` from a `match` over a pool slot.
HELD = {"Ok", "Some"}

#: Class messages that answer the class they are sent to, which no row of `objc2_metal.tsv` can
#: say: `objc2` spells the class object `T::class()` and a fresh instance `T::alloc()`, and neither
#: selector is one `objc2-metal` binds on a Metal type. A receiver made by either is that type, so
#: a selector sent on by hand -- `msg_send![MTLDescriptor::class(), ..]` -- resolves against it.
ITSELF = {"class", "alloc"}

#: Calls that answer what they were given: an `Option` or `Result` adapter around a Metal object,
#: which the suite's leg uses to turn a `nil` into an error. Following them is what lets
#: `device.newCommandQueue().ok_or_else(..)?` type as an `MTLCommandQueue`.
THROUGH = {
    "ok_or",
    "ok_or_else",
    "unwrap",
    "expect",
    "clone",
    "as_ref",
    "as_deref",
    "to_owned",
    "take",
    # A collection's element, under the same convention: `UNWRAP` reads `Vec<Retained<T>>` as `T`,
    # so a slot of the leg's own pools answers what the pool holds. No Metal type binds any of
    # these three names, so none of them shadows a selector.
    "get",
    "first",
    "last",
    "iter",
    "into_iter",
    "retain",
    "map_err",
    "unwrap_or_else",
    "borrow",
    # A pool lookup and an `Option`'s contents: `self.libraries.get(i)`, `self.encoder.take()`.
    "get",
    "take",
    "first",
    "last",
    "pop",
    "as_mut",
}


def blanked(text: str) -> str:
    """`text` with comments and string literals replaced by spaces of the same length.

    Keeping the length means every offset still points where it did, so a brace walk over the
    result can be read against the original text.
    """
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        two = text[i : i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif two == "/*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and text[i - 1 : i + 1] != "*/":
                out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                i += 1
        elif text[i] == '"':
            out[i] = " "
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    out[i] = " "
                    i += 1
                if i < n:
                    out[i] = " "
                    i += 1
            # The quote that closes it, so the next one found opens a string rather than closing
            # the one just left.
            if i < n:
                out[i] = " "
                i += 1
        elif text[i] == "'" and text[i + 1 : i + 2] and text[i + 2 : i + 3] == "'":
            # A character literal. A lifetime (`'a`) is one quote and is left alone.
            out[i] = out[i + 1] = out[i + 2] = " "
            i += 3
        else:
            i += 1
    return "".join(out)


def without_test_modules(text: str) -> str:
    """`text` with the body of every `#[cfg(test)] mod ..` replaced by spaces.

    A unit test is not the Metal leg. It is the suite's own code **about** the suite, and its
    receivers are the suite's own types, whose methods share names with Metal's selectors:
    `case.name()` answers a `Case`'s name and not `MTLFunction`'s, `column.size()` a byte width and
    not `MTLBuffer`'s. Read as a Metal call site, each such name costs its selector a receiver this
    cannot type, and the document then says of the site that one Metal item received it -- which
    for `case.name()` is false, and for 58 of the 130 sites that resolved to no item was the whole
    of what they were.

    Offsets are kept as [`blanked`] keeps them, so a span of the result reads against the original.
    """
    out = list(text)
    for match in re.finditer(r"#\s*\[\s*cfg\s*\(\s*test\s*\)\s*\]", text):
        rest = text[match.end() :]
        mod = re.match(r"\s*(?:pub\s+)?mod\s+[A-Za-z_][A-Za-z0-9_]*\s*", rest)
        if not mod:
            # Not a module: `#[cfg(test)]` on a function, a `use` or an item, which no loop here
            # reads as a call site of its own.
            continue
        at = match.end() + mod.end()
        if text[at : at + 1] == "{":
            # `block` answers a half-open span, so its end is already past the closing brace.
            stop = block(text, at)[1]
        elif text[at : at + 1] == ";":
            stop = at + 1
        else:
            continue
        for i in range(match.start(), min(stop, len(text))):
            if not text[i].isspace():
                out[i] = " "
    return "".join(out)


def test_module_paths(src: Path) -> set[Path]:
    """Every source file the crate declares under `#[cfg(test)] mod ..;`.

    The module's own file is not in the leg for the same reason its body would not be: it is the
    suite's code about the suite. Which file that is comes from the declaration, not from a name
    this script chooses, so a test module called anything else is excluded the same way.
    """
    out: set[Path] = set()
    for path in sorted(src.rglob("*.rs")):
        text = blanked(path.read_text())
        for match in re.finditer(
            r"#\s*\[\s*cfg\s*\(\s*test\s*\)\s*\]\s*(?:pub\s+)?mod\s+"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*;",
            text,
        ):
            name = match.group(1)
            here = path.parent if path.name in ("lib.rs", "mod.rs") else path.with_suffix("")
            out.add(here / f"{name}.rs")
            out.add(here / name / "mod.rs")
    return out


def block(text: str, open_at: int) -> tuple[int, int]:
    """The half-open span of the brace block that opens at or after `open_at`."""
    start = text.find("{", open_at)
    if start < 0:
        return (len(text), len(text))
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return (start, i + 1)
    return (start, len(text))


def metal_type(decl: str) -> str:
    """The Metal class or protocol a type declaration is, or `""`.

    `&ProtocolObject<dyn MTLDevice>`, `Retained<MTLTextureDescriptor>` and
    `Vec<Retained<ProtocolObject<dyn MTLBuffer>>>` each name one Metal type; anything naming none,
    or more than one, is not a receiver this follows.
    """
    bare = UNWRAP.sub(" ", decl)
    names = [w for w in IDENT.findall(bare) if w.startswith("MTL")]
    return names[0] if len(names) == 1 and len(set(names)) == 1 else ""


@dataclass
class Api:
    """What `objc2-metal` binds: which type answers which selector, and with what."""

    #: `(type, rust name)` to the selector it sends.
    selector: dict[tuple[str, str], str]
    #: `(type, rust name)` to the Metal type the call answers, where it answers one.
    answers: dict[tuple[str, str], str]
    #: A type to the types it inherits from, nearest first.
    supertypes: dict[str, list[str]]
    #: Every Objective-C type `objc2-metal` binds, to whether it is a `class` or a `protocol`.
    #: A name outside this is not a Metal type at all, however it is spelled: a Rust struct the
    #: crate also defines (`MTLSize`), a bitflags set (`MTLBlitOption`) or an enum.
    kinds: dict[str, str]

    @classmethod
    def read(cls, path: Path) -> "Api":
        selector: dict[tuple[str, str], str] = {}
        answers: dict[tuple[str, str], str] = {}
        supertypes: dict[str, list[str]] = {}
        kinds: dict[str, str] = {}
        section = ""
        for line in path.read_text().splitlines():
            if line.startswith("#"):
                section = line
                continue
            fields = line.split("\t")
            if "supertypes" in section:
                supertypes[fields[0]] = [f for f in fields[2].split(" ") if f]
                kinds[fields[0]] = fields[1]
            else:
                ty, sel, rust, answered = fields
                selector[(ty, rust)] = sel
                if answered:
                    answers[(ty, rust)] = answered
        return cls(selector, answers, supertypes, kinds)

    def declares(self, ty: str) -> bool:
        """Whether `objc2-metal` binds `ty` as an Objective-C class or protocol."""
        return ty in self.kinds

    def chain(self, ty: str) -> list[str]:
        """`ty` and every type it inherits from, nearest first."""
        out = [ty]
        i = 0
        while i < len(out):
            for parent in self.supertypes.get(out[i], []):
                if parent not in out:
                    out.append(parent)
            i += 1
        return out

    def binds(self, ty: str, rust: str) -> tuple[str, str] | None:
        """The `(declaring type, selector)` a receiver of type `ty` sends `rust` to, or `None`."""
        for owner in self.chain(ty):
            if (owner, rust) in self.selector:
                return (owner, self.selector[(owner, rust)])
        return None

    def answered(self, ty: str, rust: str) -> str:
        """The Metal type a call of `rust` on `ty` answers, or `""`."""
        for owner in self.chain(ty):
            if (owner, rust) in self.selector:
                return self.answers.get((owner, rust), "")
        return ""

    def receivers(self, rust: str) -> set[str]:
        """Every type that binds the Rust name `rust`."""
        return {ty for ty, name in self.selector if name == rust}


@dataclass
class Call:
    """One Metal call the leg's source makes."""

    #: The file it is in, relative to the crate.
    file: str
    #: The function that holds it.
    function: str
    #: The gates around it, outermost first: a `match` arm's variant as `Enum::Variant`, and a
    #: guard on one field of the suite's own vocabulary as `field:name`.
    gates: tuple[str, ...]
    #: The Rust name, which is the selector with `:` as `_`.
    rust: str
    #: The receiver's type, or `""` where it could not be typed.
    receiver: str
    #: The SDK item, as `(declaring type, selector)`, where the receiver typed.
    item: tuple[str, str] | None


@dataclass
class Leg:
    """Every Metal call the leg makes, and what the typing could not reach."""

    calls: list[Call] = field(default_factory=list)
    #: Call sites this attributes to no item although some Metal type binds the selector, as
    #: `(file, function, rust name, receiver text)`. The call is real and one of those types
    #: received it; which one is what the typing could not say, so these are the sites that hide
    #: items, and the candidates are enumerable from `receivers`.
    unresolved: list[tuple[str, str, str, str]] = field(default_factory=list)
    #: Call sites whose selector `objc2-metal` binds on no Metal type at all, as
    #: `(file, function, rust name, receiver text)`: `alloc` and `class`, which are `NSObject`'s.
    #: There is no item to hide, so these are counted apart from `unresolved`.
    foreign: list[tuple[str, str, str, str]] = field(default_factory=list)
    #: Hand-sent call sites whose selector `objc2-metal` binds nowhere, as
    #: `(file, function, rust name, selector)`. The crate has no name for the selector, which is
    #: why the leg sends it through `msg_send!`, so no receiver typing can name a container for it
    #: and no item of this map can be credited. Apart from `foreign` because the reason differs:
    #: `alloc` is `NSObject`'s and belongs to no Metal container, while this is a Metal selector
    #: with a container the crate does not bind. The selector travels with the site so the caller
    #: can hold the pile to the one thing that would make it dishonest -- a coverage-map row for
    #: one of these selectors, which would be an item the suite exercises and nothing counts.
    private: list[tuple[str, str, str, str]] = field(default_factory=list)
    #: Call sites whose receiver typed to something that does not bind the selector: a bug in this
    #: typing, never a Metal call.
    mistyped: list[tuple[str, str, str, str]] = field(default_factory=list)
    #: Call sites whose receiver did not type but whose Rust name only one type in `objc2-metal`
    #: binds, so the receiver is that type whatever made it.
    by_uniqueness: list[tuple[str, str, str]] = field(default_factory=list)
    #: Which functions call which, by name, for the reachability closure.
    calls_function: dict[str, set[str]] = field(default_factory=dict)
    #: The gates each function's calls to other functions sit under, the same two kinds as
    #: [`Call.gates`][Call]: what a case must name, and what it must state, to reach through here.
    function_gates: dict[tuple[str, str], set[tuple[str, ...]]] = field(default_factory=dict)


@dataclass
class Function:
    """A function of the source, with the span of its body."""

    name: str
    #: The `impl` header it is in, or `""` at module level.
    impl: str
    file: str
    signature: str
    start: int
    end: int


def body_brace(text: str, after: int) -> int:
    """Where the body of the `fn` whose name ends at `after` opens, or `-1` for a declaration.

    The signature is skipped by **bracket depth**, not by looking for the first `{` and treating a
    `;` before it as the end of a declaration. A parameter of an array type carries a `;` inside
    its brackets — `imageblock: [u32; 3]` — and reading that `;` as the end of a declaration drops
    the whole function, with every Metal call it makes. Measured when this was written: it dropped
    fourteen functions of the Metal side, `texture_2d`, `read_texture`, `replace_region`,
    `dispatch`, `dispatch_threads` and `tile_pass` among them, so their calls were attributed to no
    item at all and were missing from the unresolved count too.

    `->` is skipped as a unit, because its `>` is not a closing bracket.
    """
    depth = 0
    i = after
    while i < len(text):
        two = text[i : i + 2]
        if two == "->":
            i += 2
            continue
        char = text[i]
        if char in "([<":
            depth += 1
        elif char in ")]>":
            depth = max(0, depth - 1)
        elif depth == 0 and char == "{":
            return i
        elif depth == 0 and char == ";":
            return -1
        i += 1
    return -1


def functions(text: str, file: str) -> list[Function]:
    """Every function the (blanked) source declares, with its body's span."""
    out: list[Function] = []
    impls: list[tuple[str, int]] = []
    for match in re.finditer(r"\bimpl\b[^{;]*\{", text):
        header = match.group(0)[:-1].strip()
        span = block(text, match.start())
        impls.append((header, span[1]))
        impls[-1] = (header, span[1])
    for match in re.finditer(r"\bfn\s+([A-Za-z_][A-Za-z0-9_]*)", text):
        open_at = body_brace(text, match.end())
        if open_at < 0:
            # A declaration with no body (a trait's required method).
            continue
        start, end = block(text, match.end())
        here = [h for h, stop in impls if h and match.start() < stop and start < stop]
        out.append(
            Function(
                name=match.group(1),
                impl=min(here, key=len) if here else "",
                file=file,
                signature=text[match.end() : start],
                start=start,
                end=end,
            )
        )
    return out


def arms_of(text: str, start: int, end: int, enums: set[str]) -> list[tuple[int, int, str]]:
    """Every `match` arm in `text[start:end]`, with the variants of `enums` its pattern names.

    Read from the `match` keyword, not from the variant names: a call inside an arm is only gated
    by the arm it is in, and `Failure::named(..)` before an unrelated `=>` is not an arm at all.
    An arm matching several variants (`A | B`) is attributed to each, and a nested `match` is
    found by this same walk, so a call carries every arm around it.
    """
    out: list[tuple[int, int, str]] = []
    for match in re.finditer(r"\bmatch\b", text[start:end]):
        at = start + match.start()
        open_at, close = block(text, at)
        if close > end:
            continue
        i = open_at + 1
        while i < close - 1:
            arrow = _at_depth(text, i, close - 1, "=>")
            if arrow < 0:
                break
            pattern = text[i:arrow]
            body_end = _arm_end(text, arrow + 2, close - 1)
            for name in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)::([A-Za-z_][A-Za-z0-9_]*)", pattern):
                if name.group(1) in enums:
                    out.append((arrow, body_end, f"{name.group(1)}::{name.group(2)}"))
            i = body_end + 1
    return out


#: How a gate on a field of the suite's own vocabulary is spelled, so a reader of a gate set can
#: tell it from a `match` arm's `Enum::Variant` and strip it where only the variants are wanted.
GATE_FIELD = "field:"

#: The `if` conditions whose body is reached only where the field is **stated**, read from the
#: condition text between `if` and the block it opens. An `Option` unwrapped, one asked whether it
#: is there, and a collection asked whether it is not empty: in each the field names what the
#: case had to state for the call inside to happen at all.
#:
#: A bare boolean field (`if descriptor.read_only {`) is **not** one of these. Its body is reached
#: on one of the two values the field can hold, and which value a case states is nowhere in the
#: text this reads -- the same limit that keeps a field axis from carrying a two-values check --
#: so reading it as a gate would refuse a case that states the field and sets it false.
STATED = (
    # `if let Some(x) = pass.visibility`, `if let Some(depth) = &pass.depth`, and the same with an
    # adapter on the end: `if let Some(label) = self.pass.label.as_ref()`.
    re.compile(
        r"^let\s+(?:Some|Ok)\s*\(.*\)\s*=\s*&?\s*(?:mut\s+)?[A-Za-z_][\w.]*?\.(\w+)\s*"
        r"(?:\.\s*(?:as_ref|as_deref|as_mut|clone|to_owned)\s*\(\s*\))?$"
    ),
    re.compile(r"^&?\s*[A-Za-z_][\w.]*?\.(\w+)\s*\.\s*is_some\s*\(\s*\)$"),
    re.compile(r"^!\s*&?\s*[A-Za-z_][\w.]*?\.(\w+)\s*\.\s*is_empty\s*\(\s*\)$"),
)


def _condition_end(text: str, start: int, end: int) -> int:
    """Where the `if` condition that starts at `start` ends: the `{` it opens, or `-1`.

    Only round and square brackets nest here: a `{` at bracket depth zero is the body's, and one
    inside a call (`if let Some(x) = make(Thing { a: 1 })`) is not.
    """
    depth = 0
    for i in range(start, end):
        c = text[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "{" and depth == 0:
            return i
        elif c == ";" and depth == 0:
            # A `let .. else` or a statement boundary: this `if` opens no block here.
            return -1
    return -1


def guards_of(text: str, start: int, end: int) -> list[tuple[int, int, str]]:
    """Every guard in `text[start:end]` on a field of the suite's own vocabulary, with its span.

    The gate is the **field's name**, as `field:name`, and the span is the guarded block alone: an
    `else` branch opens its own block, which this one does not cover, so a call Metal makes only
    where the field is absent carries no gate for it.

    This is what makes an item's attribution per **field** rather than per shared encoder. The
    Metal leg builds one descriptor in one function, setting each field under its own guard, so
    every case that calls that encoder reaches every call site in it; the guard says which of them
    the case's own data could ever have taken.

    The field is read by name and not by the struct it belongs to, because the typing here follows
    Metal's types and not the suite's. Two vocabulary structs with a field of one name are read as
    one gate, which is generous in the safe direction: it can let a declaration through, never
    refuse a true one.
    """
    out: list[tuple[int, int, str]] = []
    for match in re.finditer(r"\bif\s", text[start:end]):
        at = start + match.end()
        brace = _condition_end(text, at, end)
        if brace < 0:
            continue
        condition = " ".join(text[at:brace].split())
        for shape in STATED:
            found = shape.match(condition)
            if found:
                out.append((brace, block(text, brace)[1], GATE_FIELD + found.group(1)))
                break
    return out


def _at_depth(text: str, start: int, end: int, want: str) -> int:
    """Where `want` next appears in `text[start:end]` at bracket depth zero."""
    depth = 0
    i = start
    while i < end:
        c = text[i]
        if c in "{([":
            depth += 1
        elif c in "})]":
            depth -= 1
        elif depth == 0 and text[i : i + len(want)] == want:
            return i
        i += 1
    return -1


def _arm_end(text: str, start: int, end: int) -> int:
    """Where the arm body that starts at `start` ends: its block, or the comma after it."""
    i = start
    while i < end and text[i].isspace():
        i += 1
    if i < end and text[i] == "{":
        return block(text, i)[1]
    comma = _at_depth(text, i, end, ",")
    return comma if comma >= 0 else end


#: The keywords an expression cannot continue through. A receiver's text is read backwards from
#: its `.`, and `rustfmt` breaks a chain across lines, so whitespace has to continue the walk --
#: which made the walk read the keyword before the receiver as the head of it.
KEYWORDS = frozenset(
    "if else match while for in loop let return move unsafe break continue as impl fn where "
    "const static pub struct enum trait use mod type mut ref dyn".split()
)


def _skipped_back(text: str, at: int, floor: int) -> int:
    """`at` moved back over whitespace, no further than `floor`."""
    while at > floor and text[at - 1].isspace():
        at -= 1
    return at


def _block_start(text: str, close_at: int, floor: int) -> int:
    """Where the brace block whose `}` is at `close_at` opens, or `-1`.

    `floor` bounds the scan, which is why `receiver_of` takes one: the text is the whole file, and
    a walk that reads back to its start for every receiver ending in a block is quadratic in the
    file (measured: the derivation stopped finishing at all).
    """
    depth = 0
    for i in range(close_at, floor - 1, -1):
        if text[i] == "}":
            depth += 1
        elif text[i] == "{":
            depth -= 1
            if depth == 0:
                return i
    return -1


def receiver_of(text: str, at: int, floor: int = 0) -> str:
    """The text of the receiver expression whose `.` is at `at`, read back no further than `floor`.

    Whitespace is part of an expression because `rustfmt` breaks a call chain across lines, so a
    receiver is very often the line above the `.`; a newline is only an end when what precedes it
    is not an expression at all.

    Nothing here slices the text before `at`. The walk is over a whole file, so a prefix copy -- or
    a regex anchored at the end of one -- costs the length of everything above the call site, and
    with one per receiver the derivation stopped finishing at all.
    """
    i = at
    depth = 0
    while i > floor:
        c = text[i - 1]
        if c in ")]":
            depth += 1
        elif c in "([":
            if depth == 0:
                break
            depth -= 1
        elif c == "}":
            # An `unsafe { .. }` block is an expression, and the one the leg reads a command
            # buffer's status through inside a completion handler: `unsafe { own.as_ref() }`.
            # `expression` already reads such a block as a parenthesised expression, so the walk
            # continues over it; any other block ends the receiver.
            open_at = _block_start(text, i - 1, floor)
            if depth or open_at < floor:
                break
            j = _skipped_back(text, open_at, floor)
            if text[j - len("unsafe") : j] != "unsafe":
                break
            i = j - len("unsafe")
            continue
        elif c == "{":
            break
        elif c == ":":
            # The `:` of a path continues the expression; a lone one does not. It labels the field
            # of a struct literal (`vertex_function: vertex.map(..)`), and read as part of the
            # receiver it left six `MTLFunction` sends with no receiver this could type.
            if text[i - 2 : i - 1] != ":" and text[i : i + 1] != ":":
                break
        elif c.isspace():
            j = _skipped_back(text, i, floor)
            k = j
            while k > floor and _word(text[k - 1]):
                k -= 1
            # Whitespace continues a chain only where an expression precedes it, and a keyword is
            # not one: `if binding.r#type()` and `match self.device.newRenderPipelineState..` read
            # their keyword as the head of the receiver and typed neither.
            if text[k:j] in KEYWORDS:
                # The receiver starts after the keyword, not at it.
                i = j
                break
            if depth == 0 and not (j > floor and (text[j - 1].isalnum() or text[j - 1] in "_)]?")):
                break
        elif depth == 0 and not (c.isalnum() or c in "_.&*?"):
            break
        i -= 1
    out = " ".join(text[i:at].split())
    while out.startswith("["):
        # An attribute before the receiver (`#[allow(deprecated)] descriptor`). The walk stops at
        # the `#`, which leaves the attribute's own brackets at the head of the text.
        depth = 0
        cut = -1
        for k, c in enumerate(out):
            if c == "[":
                depth += 1
            elif c == "]":
                depth -= 1
                if depth == 0:
                    cut = k
                    break
        if cut < 0:
            break
        out = out[cut + 1 :].strip()
    return out


class Typing:
    """What each name in one function's body is.

    A name is typed by the `let` that binds it **nearest before** the use, so a body that reuses
    one name for two objects — `let attachment = ...depthAttachment()` after
    `let attachment = ...colorAttachments()...` — types each use as what it was last made from
    rather than as whichever binding the file happens to end with.
    """

    def __init__(
        self,
        api: Api,
        fields: dict[str, str],
        locals_: dict[str, str] | None = None,
        variants: dict[str, str] | None = None,
        returns: dict[str, list[str]] | None = None,
    ):
        self.api = api
        self.fields = fields
        #: The Metal type each function of the leg itself answers, where it answers one. A pool
        #: getter (`self.held(handle)`) is how most of this source reaches a Metal object.
        self.locals = locals_ or {}
        #: Each `Enum::Variant` of the leg's own enums to the type of the value it holds, which is
        #: what types a name a `match` arm binds.
        self.variants = variants or {}
        #: Each function of the leg whose declared return is a **tuple**, to that tuple's parts,
        #: which is what types one slot of `let (command_buffer, encoder) = self.pass(..)?`.
        self.returns = returns or {}
        #: Each name's bindings, as `(offset, declared type, initializer)`, in source order.
        self.bindings: dict[str, list[tuple[int, str, str]]] = {}
        #: Parameters, which hold for the whole body.
        self.names: dict[str, str] = {}
        #: Where the expression being typed sits, so a binding after it does not answer it.
        self.at = 0
        #: The closures being typed, so one that calls itself answers nothing rather than looping.
        self.inside: set[str] = set()
        #: Each function of the leg that takes a callable, to the Metal type it hands it.
        self.closures: dict[str, str] = {}
        #: The start of the body being typed, which bounds every walk back over its text.
        self.floor = 0

    def closure_params(self, text: str, start: int, end: int) -> None:
        """Take the type of every closure parameter in `text[start:end]`.

        A closure is how this source hands one encoder's work to two call sites, so its parameters
        type as many receivers as a function's do. Two things say what a parameter is, and neither
        is at the parameter:

        * its own declaration, where it has one. Which `|` opens a list is read structurally
          ([`closure_bars`]) rather than by pairing every `|` in the body, because `|` is also
          bitwise or and `||` is an empty list: paired left to right, one
          `MTLResourceUsage::Read | MTLResourceUsage::Write` swallowed the next real list and the
          declared `&ProtocolObject<dyn MTLCommandBuffer>` of eight receivers was lost;
        * otherwise the call the closure is an argument of. A combinator of [`OF_ELEMENT`] hands it
          one element of its receiver, and a function of the leg hands it whatever that function's
          own signature declares (`Side::encode` names the encoder its callers never spell).
        """
        for open_bar, close_bar in closure_bars(text, start, end):
            parts = [part for part in _split(text[open_bar + 1 : close_bar]) if part.strip()]
            given = self.closure_given(text, open_bar)
            for i, part in enumerate(parts):
                name, decl = _annotated(part)
                name = name.strip().removeprefix("mut ").strip().lstrip("&").strip()
                if not IDENT.fullmatch(name):
                    continue
                ty = metal_type(decl) if decl.strip() else (given if i == 0 else "")
                if ty:
                    self.bind(open_bar, name, ty, "")

    def closure_given(self, text: str, at: int) -> str:
        """The Metal type the call around the closure at `at` hands its first parameter."""
        head, dot = _call_head(text, at)
        if head in self.closures:
            return self.closures[head]
        if dot >= 0 and head in OF_ELEMENT:
            return self.typed(receiver_of(text, dot, self.floor), dot)
        return ""

    def param_types(self, signature: str) -> None:
        """Take the declared types of a signature's parameters."""
        depth = 0
        start = signature.find("(")
        parts: list[str] = []
        piece = ""
        for c in signature[start + 1 :]:
            if c in "(<[":
                depth += 1
            elif c in ")>]":
                if depth == 0:
                    break
                depth -= 1
            if c == "," and depth == 0:
                parts.append(piece)
                piece = ""
            else:
                piece += c
        parts.append(piece)
        for part in parts:
            if ":" not in part:
                continue
            name, decl = part.split(":", 1)
            name = name.strip().removeprefix("mut ").strip()
            if IDENT.fullmatch(name):
                ty = metal_type(decl)
                if ty:
                    self.names[name] = ty

    def expression(self, expr: str) -> str:
        """The Metal type `expr` is, or `""`."""
        expr = _unsafe_as_parens(" ".join(expr.split()))
        expr = expr.strip("&*").strip().rstrip("?;").strip()
        if not expr:
            return ""
        if expr.startswith("match") and not _word(expr[5:6]):
            return self.match_answer(expr)
        if expr.endswith(")"):
            open_at = _matching(expr)
            if open_at >= 0:
                head = expr[:open_at].strip()
                sep, recv, called = _tail_call(head)
                called = called.strip()
                if sep == "::":
                    ty = recv.strip().strip("&*<>").split("::")[-1].strip()
                    if not ty.startswith("MTL"):
                        return ""
                    if called in ITSELF:
                        return ty
                    return self.api.answered(ty, called)
                if sep == ".":
                    if called in THROUGH:
                        # An `Option` or `Result` adapter answers what it was given.
                        return self.expression(recv)
                    if recv.strip() == "self":
                        return self.locals.get(called, "")
                    owner = self.expression(recv)
                    return self.api.answered(owner, called) if owner else ""
                if not head:
                    # A parenthesised expression, or the block of an `unsafe` the next rule turned
                    # into one.
                    return self.expression(expr[open_at + 1 : -1])
                if head.strip() in HELD:
                    # `Ok(..)` and `Some(..)` answer what they hold, as the adapters that unwrap
                    # them answer what they held.
                    return self.expression(expr[open_at + 1 : -1])
                if head.strip() in self.locals:
                    return self.locals[head.strip()]
                return self.closure_answer(head.strip())
        if expr.endswith("]"):
            open_at = expr.rfind("[")
            if open_at > 0:
                return self.expression(expr[:open_at])
        if expr.startswith("MTL") and "::" not in expr and IDENT.fullmatch(expr):
            return expr
        steps = [piece.strip() for piece in expr.split(".")]
        head = steps[0].split("[")[0].strip()
        owner = self.fields if head == "self" else None
        if owner is not None:
            steps = steps[1:]
            if not steps:
                return ""
            ty = self.fields.get(steps[0].split("[")[0], "")
            steps = steps[1:]
        else:
            ty = self.name(head)
            steps = steps[1:]
        for step in steps:
            if not ty:
                return ""
            ty = self.api.answered(ty, step.split("[")[0].strip())
        return ty

    def bind(self, at: int, name: str, decl: str, expr: str) -> None:
        """Take `let name: decl = expr`, bound at offset `at`."""
        self.bindings.setdefault(name, []).append((at, decl, expr))

    def bind_pattern(self, at: int, pattern: str, decl: str, expr: str) -> None:
        """Take a binding of `pattern`, whatever shape the pattern is.

        A **pattern**, not a name, which is what the leg writes: a command buffer and its encoder
        come out of one `let (command_buffer, encoder) = self.pass(targets)?`, two queues out of
        one `let (waiting_queue, signalling_queue) = (queue()?, queue()?)`, and an open encoder out
        of one `OpenEncoder::Blit(encoder) =>`. Read as names only, every one of those receivers
        was unbound, and the calls on them -- `commit`, `waitUntilCompleted`, `status`,
        `endEncoding`, `commandBuffer` -- resolved to no item: 38 of the 72 sites that remained.

        Each slot takes its own type. A tuple pattern takes the annotation's slot where the
        annotation is a tuple, the expression's slot where the expression is one, and otherwise the
        slot of the tuple the called function declares it answers. `Some(p)` and `Ok(p)` are
        transparent, as [`THROUGH`] makes the same adapters transparent in an expression. A
        variant's payload takes the type the enum declares for it. A slot this cannot name binds
        nothing, rather than binding a name to the whole of what the pattern destructured.
        """
        pattern = pattern.strip()
        while True:
            held = re.sub(r"^(?:&\s*|mut\s+|ref\s+)", "", pattern)
            if held == pattern:
                break
            pattern = held
        if not pattern or pattern == "_":
            return
        if IDENT.fullmatch(pattern):
            self.bind(at, pattern, decl, expr)
            return
        if pattern.startswith("(") and pattern.endswith(")"):
            parts = [part.strip() for part in _split(pattern[1:-1])]
            while parts and not parts[-1]:
                parts.pop()
            decls = _tuple_parts(decl) or self.tuple_answer(expr)
            exprs = _tuple_parts(expr)
            for i, part in enumerate(parts):
                self.bind_pattern(
                    at,
                    part,
                    decls[i] if len(decls) == len(parts) else "",
                    exprs[i] if len(exprs) == len(parts) else "",
                )
            return
        held = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_:]*)\s*\((.*)\)", pattern, re.S)
        if not held:
            return
        head, inner = held.group(1), held.group(2)
        if head.split("::")[-1] in ("Some", "Ok"):
            self.bind_pattern(at, inner, decl, expr)
            return
        key = "::".join(head.split("::")[-2:])
        if key in self.variants and len(_split(inner)) == 1:
            self.bind_pattern(at, inner, self.variants[key], "")

    def match_answer(self, expr: str) -> str:
        """What the `match` expression `expr` answers: the first of its arms that types.

        The first is enough because every arm of a `match` answers one type in Rust, so where any
        of them names a Metal object they all do; the others are `Err(..)` and `None`, which name
        none. The leg makes a command queue this way (`match max { None => .., Some(..) => .. }`)
        and reads every pool slot this way.
        """
        open_at, close = block(expr, 0)
        i = open_at + 1
        while i < close - 1:
            arrow = _at_depth(expr, i, close - 1, "=>")
            if arrow < 0:
                return ""
            body_end = _arm_end(expr, arrow + 2, close - 1)
            ty = self.expression(expr[arrow + 2 : body_end])
            if ty:
                return ty
            i = body_end + 1
        return ""

    def closure_answer(self, name: str) -> str:
        """What a call of the closure bound to `name` answers, or `""`.

        The leg makes a second command queue by binding a closure and calling it twice --
        `let queue = || self.device.newCommandQueue().ok_or_else(..); let (a, b) = (queue()?,
        queue()?)` -- which is how both queues of every two-queue probe are made. The call names no
        type and the closure declares no return, so the only statement of what it answers is its
        own tail expression.
        """
        if name in self.inside:
            return ""
        before = [held for held in self.bindings.get(name, []) if held[0] < self.at]
        if not before:
            return ""
        expr = max(before, key=lambda held: held[0])[2].strip()
        expr = expr.removeprefix("move").strip()
        if not expr.startswith("|"):
            return ""
        bar = expr.find("|", 1)
        if bar < 0:
            return ""
        body = expr[bar + 1 :].strip()
        if body.startswith("->"):
            # A closure that declares what it answers says it outright, which is how the leg's own
            # `named` helper states the function it looks up.
            at = body.find("{")
            return metal_type(body[2:at] if at >= 0 else body[2:])
        if body.startswith("{"):
            body = body[1 : block(body, 0)[1] - 1]
        tail = [piece for piece in _statements(body) if piece.strip()]
        if not tail:
            return ""
        self.inside.add(name)
        try:
            return self.expression(tail[-1])
        finally:
            self.inside.discard(name)

    def tuple_answer(self, expr: str) -> list[str]:
        """The parts of the tuple a call of one of the leg's own functions answers, or `[]`."""
        expr = _unsafe_as_parens(" ".join(expr.split())).strip("&*").strip().rstrip("?;").strip()
        if not expr.endswith(")"):
            return []
        open_at = _matching(expr)
        if open_at < 0:
            return []
        return self.returns.get(expr[:open_at].strip().rpartition(".")[2].strip(), [])

    def locals_of(self, text: str, start: int, end: int) -> None:
        """Take every binding the body `text[start:end]` makes, in source order.

        Four constructs bind a name to a Metal object here, and reading only the first left the
        other three unspelled: a `let` (including `if let`, `while let` and `let .. else`), a `for`
        over something that holds Metal objects, and a `match` arm.

        A `for` binds its name to the **iterable**, which is how its element is typed: `UNWRAP`
        reads `Vec<Retained<T>>` as `T` and `objc2_metal.tsv` records an `NSArray<T>` getter as
        answering `T`, so `for binding in &bindings` types each binding as what the array holds.
        """
        for match in re.finditer(r"\blet\s+([^=;]*?)\s*=(?!=)", text[start:end]):
            at = start + match.end()
            before = text[: start + match.start()].rstrip()
            block_form = bool(re.search(r"\b(?:if|while)$", before))
            pattern, decl = _annotated(match.group(1))
            stop = _initializer_end(text, at, end, block_form)
            self.bind_pattern(start + match.start(), pattern, decl, text[at:stop])
        for match in re.finditer(r"\bfor\s+([^;{]*?)\s+in\s+", text[start:end]):
            at = start + match.end()
            stop = _initializer_end(text, at, end, True)
            self.bind_pattern(start + match.start(), match.group(1), "", text[at:stop])
        for match in re.finditer(r"\bmatch\b", text[start:end]):
            at = start + match.start()
            open_at, close = block(text, at)
            if close > end:
                continue
            subject = text[at + len("match") : open_at]
            i = open_at + 1
            while i < close - 1:
                arrow = _at_depth(text, i, close - 1, "=>")
                if arrow < 0:
                    break
                for alternative in text[i:arrow].split("|"):
                    self.bind_pattern(arrow, alternative, "", subject)
                i = _arm_end(text, arrow + 2, close - 1) + 1

    def name(self, name: str) -> str:
        """The Metal type `name` holds where it is being used, or `""`."""
        before = [b for b in self.bindings.get(name, []) if b[0] < self.at]
        if before:
            # The nearest binding before the use, by offset rather than by the order they were
            # taken in: the constructs that bind a name are read one kind at a time, so a `for` or
            # a `match` arm is taken after a `let` that sits below it in the source.
            at, decl, expr = max(before, key=lambda held: held[0])
            ty = metal_type(decl) if decl else ""
            if ty:
                return ty
            here, self.at = self.at, at
            try:
                return self.expression(expr)
            finally:
                self.at = here
        return self.names.get(name, "")

    def typed(self, expr: str, at: int) -> str:
        """The Metal type the expression `expr`, which sits at offset `at`, is."""
        self.at = at
        return self.expression(expr)


def _unsafe_as_parens(expr: str) -> str:
    """`expr` with each `unsafe { .. }` block written as `( .. )`.

    An `unsafe` block is an expression here — `unsafe { device.newX(..) }.ok_or_else(..)?` — so
    reading its braces as parentheses lets one rule type the block and the chain after it.
    """
    out = expr
    while True:
        at = out.find("unsafe")
        if at < 0:
            return out
        rest = out[at + len("unsafe") :]
        if not rest.lstrip().startswith("{"):
            return out
        open_at = at + len("unsafe") + rest.index("{")
        depth = 0
        close = len(out) - 1
        for i in range(open_at, len(out)):
            if out[i] == "{":
                depth += 1
            elif out[i] == "}":
                depth -= 1
                if depth == 0:
                    close = i
                    break
        out = out[:at] + "(" + out[open_at + 1 : close] + ")" + out[close + 1 :]


def _split(text: str) -> list[str]:
    """`text` split on the commas at depth zero."""
    out: list[str] = []
    piece = ""
    depth = 0
    for c in text:
        if c in "(<[":
            depth += 1
        elif c in ")>]":
            depth -= 1
        if c == "," and depth == 0:
            out.append(piece)
            piece = ""
        else:
            piece += c
    out.append(piece)
    return out


def _tail_call(head: str) -> tuple[str, str, str]:
    """`head` split at its last separator at depth zero, as `(separator, receiver, name)`.

    At depth zero, and that is the whole point: a `::` **inside** an argument is not the head's own
    separator. `self.pool.get(i).ok_or_else(|| Failure::named(..))` was read as a message to a type
    called `Failure`, so every chain the leg turns a `nil` into a refusal with -- a pool slot, a
    pipeline's reflection -- typed as nothing.
    """
    depth = 0
    for i in range(len(head) - 1, -1, -1):
        c = head[i]
        if c in ")]}":
            depth += 1
        elif c in "([{":
            depth -= 1
        elif depth == 0 and c == ".":
            return (".", head[:i], head[i + 1 :])
        elif depth == 0 and c == ":" and head[i - 1 : i] == ":":
            return ("::", head[: i - 1], head[i + 1 :])
    return ("", "", head)


def _matching(expr: str) -> int:
    """Where the `(` that closes at `expr`'s last character opens."""
    depth = 0
    for i in range(len(expr) - 1, -1, -1):
        if expr[i] in ")]}":
            depth += 1
        elif expr[i] in "([{":
            depth -= 1
            if depth == 0:
                return i
    return -1


def variant_fields(text: str) -> dict[str, str]:
    """Each `Enum::Variant` the source declares that holds one value, to that value's type.

    A `match` arm binds its variant's payload and says nothing about what the payload is; the type
    is in the enum. The leg's `OpenEncoder::Blit(encoder)` is the only receiver of `endEncoding` in
    `OpenEncoder::end`, and without the enum there is nothing to type it from.
    """
    out: dict[str, str] = {}
    for match in re.finditer(r"\benum\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", text):
        owner = match.group(1)
        start, end = block(text, match.start())
        for piece in _split(text[start + 1 : end - 1]):
            held = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*$", piece, re.S)
            if held and len(_split(held.group(2))) == 1:
                out[f"{owner}::{held.group(1)}"] = held.group(2)
    return out


def _tuple_parts(text: str) -> list[str]:
    """The parts of the first parenthesised group of `text` holding a comma at depth zero.

    A tuple, wherever it sits: the pattern `(command_buffer, encoder)`, the expression
    `(queue()?, queue()?)`, and the return type `Answer<(Retained<..>, Retained<..>)>` are each
    read by this, which is what lets one slot of a pattern take one slot of what made it.
    """
    for i, c in enumerate(text):
        if c != "(":
            continue
        depth = 0
        for j in range(i, len(text)):
            if text[j] in "([<":
                depth += 1
            elif text[j] in ")]>":
                depth -= 1
                if depth == 0:
                    parts = [part.strip() for part in _split(text[i + 1 : j])]
                    # A trailing comma is how `rustfmt` writes a tuple that does not fit on one
                    # line, and counted as a slot it made every such tuple one part longer than
                    # the pattern it was destructured by: `Side::pass` answers two objects and
                    # read as three slots it typed neither.
                    while parts and not parts[-1]:
                        parts.pop()
                    if len(parts) > 1:
                        return parts
                    break
    return []


def _annotated(pattern: str) -> tuple[str, str]:
    """`pattern` split into the pattern and its type annotation, which may be absent.

    The `:` of an annotation, not the `::` of a path: `Ok(MTLThing::Kind)` carries no annotation,
    and reading its second colon as one would leave a pattern of half a path.
    """
    depth = 0
    for i, c in enumerate(pattern):
        if c in "([<{":
            depth += 1
        elif c in ")]>}":
            depth -= 1
        elif c == ":" and depth == 0:
            if pattern[i + 1 : i + 2] == ":":
                continue
            if i and pattern[i - 1] == ":":
                continue
            return pattern[:i], pattern[i + 1 :]
    return pattern, ""


def _word(char: str) -> bool:
    """Whether `char` can sit inside an identifier, which `str.isalnum` says nothing about.

    `_` is the one that matters: read as a boundary it found `else` inside `ok_or_else`, and every
    initializer ending in that adapter -- the leg's way of turning a `nil` into a refusal -- was
    cut off before the call it was typed by.
    """
    return bool(char) and (char.isalnum() or char == "_")


def _initializer_end(text: str, at: int, end: int, block_form: bool) -> int:
    """Where the initializer that starts at `at` ends.

    Its `;` at depth zero, as a statement's is, and also the `else` of a `let .. else`. A
    `block_form` -- `if let` and `while let` -- has no `;` at all: its initializer ends at the `{`
    of the body it guards, and read to the `;` instead it swallowed the whole block, which is how
    `if let Some(encoder) = self.encoder.take() {` typed its encoder as nothing.
    """
    depth = 0
    i = at
    while i < end:
        c = text[i]
        if c in "([{":
            if c == "{" and depth == 0 and block_form:
                return i
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return i
            depth -= 1
        elif c == ";" and depth == 0:
            return i
        elif depth == 0 and text[i : i + 4] == "else" and not (
            _word(text[i - 1 : i]) or _word(text[i + 4 : i + 5])
        ):
            return i
        i += 1
    return end


def struct_fields(text: str, name: str) -> dict[str, str]:
    """The Metal type of each field of struct `name`, where it has one."""
    match = re.search(rf"\bstruct\s+{name}\b[^{{;]*\{{", text)
    if not match:
        return {}
    start, end = block(text, match.start())
    out: dict[str, str] = {}
    depth = 0
    piece = ""
    for c in text[start + 1 : end - 1]:
        if c in "(<[":
            depth += 1
        elif c in ")>]":
            depth -= 1
        if c == "," and depth == 0:
            if ":" in piece:
                field_name, decl = piece.split(":", 1)
                ty = metal_type(decl)
                if ty:
                    out[field_name.strip().removeprefix("pub ").strip()] = ty
            piece = ""
        else:
            piece += c
    if ":" in piece:
        field_name, decl = piece.split(":", 1)
        ty = metal_type(decl)
        if ty:
            out[field_name.strip().removeprefix("pub ").strip()] = ty
    return out


#: The combinators a function of the leg is handed to as a value, as one alternation. Every one
#: takes a callable and calls it, so a leg function named here is reached; a method that merely
#: *holds* a value would not be an edge. The list is of what this leg writes, and a combinator it
#: does not use yet costs nothing to have here.
HIGHER_ORDER = (
    "map|map_err|and_then|or_else|filter_map|flat_map|for_each|try_for_each|inspect"
    "|unwrap_or_else|ok_or_else|map_or_else|map_while|take_while|skip_while|find_map"
    "|is_some_and|is_none_or|sort_by_key|max_by_key|min_by_key|retain|partition|all|any"
)

#: Those of [`HIGHER_ORDER`] whose callable is handed one **element** of what it was called on, so
#: a closure parameter with no declared type is that element: `vertex.map(|held| held.name())`
#: hands the function the `Option` held. The ones left out hand it the **error** instead
#: (`map_err`, `or_else`, `ok_or_else`, `unwrap_or_else`, `map_or_else`), which names no Metal
#: object and must not be read as the element.
OF_ELEMENT = frozenset(
    HIGHER_ORDER.split("|")
) - {"map_err", "or_else", "ok_or_else", "unwrap_or_else", "map_or_else"}


def read_leg(api: Api, src: Path, files: list[str], enums: set[str]) -> Leg:
    """Every Metal call the leg's `files` make, typed."""
    leg = Leg()
    blank = {
        name: without_test_modules(blanked((src / name).read_text())) for name in files
    }
    fields: dict[str, str] = {}
    answers: dict[str, str] = {}
    variants: dict[str, str] = {}
    returns: dict[str, list[str]] = {}
    closures: dict[str, str] = {}
    #: Every function the leg declares, by name: what a bare path handed to a combinator has to be
    #: one of before it is read as an edge ([`HIGHER_ORDER`]).
    declared: set[str] = set()
    for name, text in blank.items():
        for struct in re.findall(r"\bstruct\s+([A-Za-z_][A-Za-z0-9_]*)", text):
            fields.update(struct_fields(text, struct))
        variants.update(variant_fields(text))
        for fn in functions(text, name):
            declared.add(fn.name)
            arrow = fn.signature.rfind("->")
            if arrow >= 0:
                ty = metal_type(fn.signature[arrow + 2 :])
                if ty:
                    answers[fn.name] = ty
                parts = _tuple_parts(fn.signature[arrow + 2 :])
                if parts:
                    returns[fn.name] = parts
            held = re.search(r"\bFn(?:Once|Mut)?\s*\(([^)]*)\)", fn.signature)
            if held:
                ty = metal_type(held.group(1))
                if ty:
                    closures[fn.name] = ty
    for name, text in blank.items():
        for fn in functions(text, name):
            gates = arms_of(text, fn.start, fn.end, enums) + guards_of(text, fn.start, fn.end)
            typing = Typing(api, fields, answers, variants, returns)
            typing.closures = closures
            typing.floor = fn.start
            typing.param_types(fn.signature)
            # Every binding of the body, so a use of a name is typed by the one nearest before it.
            # Before the closure parameters, because typing one of those reads the receiver of the
            # call that hands it its element, and that receiver is very often a local of this body.
            typing.locals_of(text, fn.start, fn.end)
            typing.closure_params(text, fn.start, fn.end)
            # `r#type`, which is how `objc2` spells a binding whose selector is a Rust keyword.
            # Without the `r#` such a call site matches nothing at all — the identifier after the
            # dot is `r`, which is not followed by `(` — so every `MTLBinding`'s and
            # `MTLArgument`'s `type` the leg reads was invisible here.
            for match in re.finditer(
                r"\.\s*(?:r#)?([A-Za-z_][A-Za-z0-9_]*)\s*\(", text[fn.start : fn.end]
            ):
                rust = match.group(1)
                at = fn.start + match.start()
                here = tuple(v for lo, hi, v in gates if lo <= at < hi)
                owners = api.receivers(rust)
                recv = receiver_of(text, at, fn.start)
                if recv == "self" or not owners:
                    # `self` is the leg's own side, never a Metal object, so a name it shares with
                    # a selector (`buffer`, `texture`) is this source's own function.
                    # Not a Metal call at all. A call to another function of the leg is an edge of
                    # the reachability graph instead.
                    leg.calls_function.setdefault(fn.name, set()).add(rust)
                    leg.function_gates.setdefault((fn.name, rust), set()).add(here)
                    continue
                ty = typing.typed(recv, at)
                if not ty and len(owners) == 1:
                    # Only one type binds the name, so no typing is needed to know the receiver.
                    ty = next(iter(owners))
                    leg.by_uniqueness.append((name, fn.name, rust))
                if not ty:
                    leg.unresolved.append((name, fn.name, rust, recv))
                    leg.calls.append(Call(name, fn.name, here, rust, "", None))
                    continue
                item = api.binds(ty, rust)
                if item is None:
                    leg.mistyped.append((name, fn.name, rust, ty))
                    leg.calls.append(Call(name, fn.name, here, rust, ty, None))
                    continue
                leg.calls.append(Call(name, fn.name, here, rust, ty, item))
            # `MTLType::selector(..)`, which is how `objc2` spells a message to the **class**: a
            # descriptor's own constructor (`MTLRenderPassDescriptor::renderPassDescriptor()`) and
            # the `init` an `alloc()` is handed. The receiver is the class the path names, so
            # nothing has to be typed; the path is resolved against what `objc2-metal` binds on
            # that class, the same way an instance send is.
            #
            # `\s*` after each `::` because a long constructor's name does not fit beside its
            # class: `MTLRasterizationRateMapDescriptor::` ends one line and
            # `rasterizationRateMapDescriptorWithScreenSize_layer(..)` begins the next, and with
            # the whitespace unmatched that call site was in no table at all -- not a call, not
            # unresolved, nowhere.
            for match in re.finditer(
                r"\b((?:[A-Za-z_][A-Za-z0-9_]*::\s*)+)(?:r#)?([A-Za-z_][A-Za-z0-9_]*)\s*\(",
                text[fn.start : fn.end],
            ):
                rust = match.group(2)
                at = fn.start + match.start()
                ty = [part for part in match.group(1).split("::") if part.strip()][-1].strip()
                if not api.declares(ty):
                    # Not an Objective-C type of `objc2-metal` at all, so this sends no message:
                    # a Rust type's associated function (`Vec::new`), an enum's variant, or a
                    # bitflags constructor whose name only looks like Metal's
                    # (`MTLBlitOption::empty`). The loop below reads it as the leg's own call.
                    continue
                here = tuple(v for lo, hi, v in gates if lo <= at < hi)
                item = api.binds(ty, rust)
                if item is None:
                    # The class named here does not bind this selector. Not `mistyped`: nothing was
                    # typed wrongly, the path says its own receiver. Which pile it belongs in is
                    # whether ANY Metal type binds the name: `alloc` and `class` are `NSObject`'s
                    # and so are an item of no Metal container, while a name some other Metal type
                    # binds means a real item received this call and the table cannot say which.
                    pile = leg.unresolved if api.receivers(rust) else leg.foreign
                    pile.append((name, fn.name, rust, ty))
                    leg.calls.append(Call(name, fn.name, here, rust, "", None))
                    continue
                leg.calls.append(Call(name, fn.name, here, rust, ty, item))
            # `msg_send![recv, label: arg, ..]`, which is how the leg sends a selector
            # `objc2-metal` binds no method for ([`hand_sent`]). The receiver is typed exactly as
            # an instance send's is; what differs is the one honest outcome below.
            for at, recv, rust, selector in hand_sent(text, fn.start, fn.end):
                here = tuple(v for lo, hi, v in gates if lo <= at < hi)
                if not api.receivers(rust):
                    # A selector `objc2-metal` binds nowhere, sent by hand because of it. Its own
                    # pile rather than `foreign`: `alloc` and `class` are `NSObject`'s and belong
                    # to no Metal container at all, while this is a Metal selector the crate has no
                    # name for, and the caller checks the coverage map for a row of it.
                    leg.private.append((name, fn.name, rust, selector))
                    leg.calls.append(Call(name, fn.name, here, rust, "", None))
                    continue
                ty = typing.typed(recv, at)
                item = api.binds(ty, rust) if ty else None
                if item is None:
                    # Not `mistyped`, even where the receiver typed to something that does not bind
                    # the selector. A hand-sent selector is often sent **because** the Rust types
                    # cannot express the receiver: `arrayLength` goes to a `MTLBinding` that the
                    # case has just read as a texture binding, which is where the item lives, and
                    # objc2 offers no cast between two protocol objects. So the typing landed on a
                    # supertype of the real receiver, which is not a typing error; the item that
                    # received the call is one of the selector's own, and this cannot say which.
                    leg.unresolved.append((name, fn.name, rust, recv))
                    leg.calls.append(Call(name, fn.name, here, rust, "", None))
                    continue
                leg.calls.append(Call(name, fn.name, here, rust, ty, item))
            # A call to another function of the leg: an edge of the reachability graph. A path
            # qualifier is kept where it is a module (`crate::metal_work::run`) or `Self`, and
            # dropped where it is a type, because `MTLTextureDescriptor::new` is the class message
            # the loop above resolves and not this source's own `new`.
            #
            # A method send is excluded by the `.` before its name, and by that alone: `rustfmt`
            # breaks a chain **before** the dot, so a continuation line reads `.label(` and the
            # dot is beside the name wherever the call sits. Measured over the whole leg: 0 call
            # sites have whitespace between the dot and the name.
            for match in re.finditer(
                r"\b((?:[A-Za-z_][A-Za-z0-9_]*::)*)([A-Za-z_][A-Za-z0-9_]*)\s*\(",
                text[fn.start : fn.end],
            ):
                rust = match.group(2)
                at = fn.start + match.start()
                if text[at - 1 : at] == "." or rust in ("fn", "if", "match", "while", "for"):
                    continue
                qualifier = [part for part in match.group(1).split("::") if part]
                if qualifier and qualifier[-1][:1].isupper() and qualifier[-1] != "Self":
                    continue
                here = tuple(v for lo, hi, v in gates if lo <= at < hi)
                leg.calls_function.setdefault(fn.name, set()).add(rust)
                leg.function_gates.setdefault((fn.name, rust), set()).add(here)
            # A function of the leg handed to a combinator as a **value**, which is an edge of the
            # same graph and was in none of the loops above: each of them needs a `(` after the
            # name, and `.map(layer_descriptor)` has none.
            #
            # It is not a cosmetic hole. `Side::rate_map` builds each layer's descriptor through
            # `.map(layer_descriptor)`, so with the edge missing the two
            # `MTLRasterizationRateLayerDescriptor` items that function reaches -- its class and
            # `initWithSampleCount:horizontal:vertical:` -- were called by a leg the rate-map
            # cases run and credited to no case module at all.
            #
            # The pattern is narrow on purpose: the receiver's method must be one of
            # [`HIGHER_ORDER`] and the argument a bare path with no call of its own, and the name
            # must be a function the leg declares. Reading any bare identifier that shares a name
            # with a function would be worse than the hole -- measured over this leg, 187 such
            # sites, nearly all of them a local named `handle`, `texture` or `buffer`.
            for match in re.finditer(
                r"\.\s*(?:" + HIGHER_ORDER + r")\s*\(\s*((?:[A-Za-z_][A-Za-z0-9_]*::)*)"
                r"([A-Za-z_][A-Za-z0-9_]*)\s*\)",
                text[fn.start : fn.end],
            ):
                rust = match.group(2)
                if rust not in declared:
                    continue
                qualifier = [part for part in match.group(1).split("::") if part]
                if qualifier and qualifier[-1][:1].isupper() and qualifier[-1] != "Self":
                    continue
                at = fn.start + match.start()
                here = tuple(v for lo, hi, v in gates if lo <= at < hi)
                leg.calls_function.setdefault(fn.name, set()).add(rust)
                leg.function_gates.setdefault((fn.name, rust), set()).add(here)
    return leg


def hand_sent(text: str, start: int, end: int) -> list[tuple[int, str, str, str]]:
    """Every `msg_send![..]` in `text[start:end]`, as `(offset, receiver, rust name, selector)`.

    `objc2-metal` does not bind every selector the SDK declares, and where it does not the leg
    sends the selector by hand. The macro carries the selector in its own syntax — a receiver, then
    one `label: argument` per part — so the name is `rasterizationRateMapDescriptorWithScreenSize`,
    `layerCount`, `layers` joined by `_`, exactly the Rust name `objc2` would generate, and a
    selector that takes nothing is the bare word after the receiver.

    Read by none of the loops above, because each needs a `(` after a name: such a site was in no
    table at all, neither called nor unresolved. Measured when this was written, eight sites, among
    them the one construction route of `MTLRasterizationRateMapDescriptor` that takes its layers
    with it — four cases compare it against Metal, and the document read it as exercised by none.
    """
    out: list[tuple[int, str, str, str]] = []
    for match in re.finditer(r"\bmsg_send!\s*\[", text[start:end]):
        at = start + match.start()
        open_at = start + match.end() - 1
        depth = 0
        close = -1
        for i in range(open_at, end):
            if text[i] in "([{":
                depth += 1
            elif text[i] in ")]}":
                depth -= 1
                if depth == 0:
                    close = i
                    break
        if close < 0:
            continue
        pieces = [piece for piece in _split(text[open_at + 1 : close]) if piece.strip()]
        if len(pieces) < 2:
            continue
        recv = " ".join(pieces[0].split())
        labels = [
            piece.split(":", 1)[0].strip() for piece in pieces[1:] if IDENT.fullmatch(
                piece.split(":", 1)[0].strip()
            )
        ]
        if len(labels) != len(pieces) - 1:
            # A part this cannot name is a part of the selector missing from the name, and a
            # shortened name would resolve to the wrong item or to none. Nothing is guessed.
            continue
        parts = [piece.split(":", 1) for piece in pieces[1:]]
        selector = (
            "".join(f"{label}:" for label in labels)
            if any(len(part) == 2 for part in parts)
            else labels[0]
        )
        out.append((at, recv, "_".join(labels), selector))
    return out


def closure_bars(text: str, start: int, end: int) -> list[tuple[int, int]]:
    """Each closure parameter list in `text[start:end]`, as the span between its two `|`.

    Read from what opens one, because `|` is three things in Rust: a parameter list, bitwise or,
    and -- doubled -- an empty parameter list. A list opens only where an expression cannot
    precede it: after `=`, `(`, `,`, `[`, `{`, `=>`, or the `move` of a `move |..|`. An `||` is a
    list of no parameters, and consuming it is what keeps the `||` of `ok_or_else(|| ..)` from
    opening a list that runs to the next real closure's own `|`.
    """
    out: list[tuple[int, int]] = []
    i = start
    while i < end:
        if text[i] != "|":
            i += 1
            continue
        before = text[start:i].rstrip()
        opens = bool(before) and (
            before[-1] in "=(,[{>" or bool(re.search(r"\bmove$", before))
        )
        if not opens or (before[-1] == "=" and before[-2:-1] in (">", "=", "!", "<")):
            i += 1
            continue
        if text[i + 1 : i + 2] == "|":
            out.append((i, i + 1))
            i += 2
            continue
        depth = 0
        j = i + 1
        while j < end:
            c = text[j]
            if c in "([{<":
                depth += 1
            elif c in ")]}>":
                if depth == 0:
                    break
                depth -= 1
            elif c == "|" and depth == 0:
                out.append((i, j))
                break
            j += 1
        i = j + 1
    return out


def _call_head(text: str, at: int) -> tuple[str, int]:
    """The name of the call whose arguments hold `at`, and the offset of its `.`, or `-1`.

    The closure at `at` is an argument of that call, which is the one place its parameter's type is
    written: `.map(|held| ..)` is handed an element of the receiver before the dot, and
    `self.encode(.., |encoder| ..)` is handed whatever `encode` declares.
    """
    depth = 0
    i = at
    while i > 0:
        c = text[i - 1]
        if c in ")]}":
            depth += 1
        elif c in "([{":
            if depth == 0:
                if c != "(":
                    return ("", -1)
                end = i - 1
                while end > 0 and text[end - 1].isspace():
                    end -= 1
                head = end
                while head > 0 and _word(text[head - 1]):
                    head -= 1
                if head == end:
                    return ("", -1)
                dot = head
                while dot > 0 and text[dot - 1].isspace():
                    dot -= 1
                return (text[head:end], dot - 1 if text[dot - 1 : dot] == "." else -1)
            depth -= 1
        i -= 1
    return ("", -1)


def _statements(text: str) -> list[str]:
    """`text` split on the `;` at depth zero: the statements of a block, its tail last."""
    out: list[str] = []
    piece = ""
    depth = 0
    for c in text:
        if c in "([{<":
            depth += 1
        elif c in ")]}>":
            depth -= 1
        if c == ";" and depth == 0:
            out.append(piece)
            piece = ""
        else:
            piece += c
    out.append(piece)
    return out


def _statement_end(text: str, at: int, end: int) -> int:
    """Where the statement that starts at `at` ends: its `;` at depth zero."""
    depth = 0
    for i in range(at, end):
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return i
            depth -= 1
        elif c == ";" and depth == 0:
            return i
    return end


if __name__ == "__main__":
    sys.exit(
        f"{Path(__file__).name} is a module of scripts/conformance-coverage, not a command: it "
        "derives one part of the document and has no output of its own. The generator is\n"
        "  python3 -I scripts/conformance-coverage/coverage.py"
    )
