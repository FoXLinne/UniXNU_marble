"""The call graph of the Metal leg, over sources whose answer is known by construction.

Run by `coverage.py --selftest`, with the option-space axis's own tests.

The reachability graph is what attributes a Metal call to a case module: an item is credited to the
families whose `Side` methods reach its call site, so an edge the walk cannot see makes a call the
suite really does make read as called by nobody. That is how the two
`MTLRasterizationRateLayerDescriptor` items the rate-map cases exercise came to sit in the
document's *attributed to no family* table: `Side::rate_map` builds each layer's descriptor through
`.map(layer_descriptor)`, and every edge the walk read needed a `(` after the name.

So both directions are held here, because the narrowing is the whole fix: a function handed to a
combinator **is** an edge, and a local variable that happens to share a name with a function is
**not**. Every test owns its inputs; nothing here reads the repository.
"""

import tempfile
import unittest
from pathlib import Path

import metal_leg

#: Every tree a test made, removed when the module is done with them.
MADE: list[tempfile.TemporaryDirectory] = []

#: The smallest `objc2-metal` map a leg can be read against: two classes, and on them a selector
#: of one part, one of several, and one each class binds alone.
API = (
    "# type\tkind\tsupertypes\n"
    "MTLThing\tclass\tNSObject\n"
    "MTLOther\tclass\tNSObject\n"
    "# type\tselector\trust\tanswers\n"
    "MTLThing\tmakeIt:\tmakeIt\t\n"
    "MTLThing\tmakeIt:with:\tmakeIt_with\t\n"
    "MTLOther\tspecial\tspecial\t\n"
    # Bound by both, so a call of it resolves only where the receiver was typed: the shape every
    # test of the typing needs, since a selector one type binds alone resolves without any.
    "MTLThing\tsetLabel:\tsetLabel\t\n"
    "MTLOther\tsetLabel:\tsetLabel\t\n"
)


def tearDownModule() -> None:
    """Remove the trees the tests built."""
    while MADE:
        MADE.pop().cleanup()


def leg_of(source: str) -> metal_leg.Leg:
    """Read a leg whose one file holds `source`."""
    held = tempfile.TemporaryDirectory()
    MADE.append(held)
    root = Path(held.name)
    (root / "objc2_metal.tsv").write_text(API)
    (root / "leg.rs").write_text(source)
    api = metal_leg.Api.read(root / "objc2_metal.tsv")
    return metal_leg.read_leg(api, root, ["leg.rs"], set())


class HigherOrderEdges(unittest.TestCase):
    """A function of the leg handed to a combinator is an edge of the graph."""

    def test_a_function_passed_to_map_is_an_edge(self):
        """`.map(f)` reaches `f`, with no call of `f` written anywhere.

        This is `Side::rate_map`'s shape, reduced: the only mention of the callee is its name as a
        value. Before the walk read it, the Metal calls inside such a function were credited to no
        case module at all.
        """
        leg = leg_of(
            """
            fn outer(things: &[u32]) -> Vec<u32> {
                things.iter().map(inner).collect()
            }
            fn inner(one: &u32) -> u32 {
                MTLThing::makeIt(*one)
            }
            """
        )
        self.assertIn("inner", leg.calls_function.get("outer", set()))

    def test_every_combinator_the_list_names_is_an_edge(self):
        """Each combinator of `HIGHER_ORDER`, held one at a time.

        A list in a regex is the shape that silently loses an alternative to a typo, and the cost
        of losing one is a hole exactly like the one this closed, so each is checked rather than
        the list being read as correct.
        """
        for combinator in metal_leg.HIGHER_ORDER.split("|"):
            leg = leg_of(
                f"""
                fn outer(thing: &Held) -{{}} u32 {{
                    thing.{combinator}(inner)
                }}
                fn inner(one: &u32) -> u32 {{
                    MTLThing::makeIt(*one)
                }}
                """.replace("-{}", "-")
            )
            self.assertIn(
                "inner",
                leg.calls_function.get("outer", set()),
                f".{combinator}(inner) is an edge",
            )

    def test_a_local_that_shares_a_functions_name_is_not_an_edge(self):
        """`.map(handle)` where `handle` is a local, not the leg's `handle` function.

        The direction that keeps the reading honest. A walk that read any bare identifier sharing
        a name with a function would add 187 such edges over this leg alone, nearly all of them a
        local named `handle`, `texture` or `buffer`, and the graph would say every `Side` method
        reaches every call site.
        """
        leg = leg_of(
            """
            fn outer(thing: &Held) -> Option<u32> {
                let handle = 3u32;
                thing.held().map(handle)
            }
            fn unrelated(one: u32) -> u32 {
                MTLThing::makeIt(one)
            }
            """
        )
        self.assertNotIn("handle", leg.calls_function.get("outer", set()))

    def test_a_type_qualified_path_is_not_an_edge(self):
        """`.map(MTLThing::makeIt)` is a message to a class, not a function of the leg.

        The same exclusion the other loops make: a path whose last qualifier is a type is Metal's
        own, and reading it as one of ours would credit the call to a function that does not exist.
        """
        leg = leg_of(
            """
            fn outer(thing: &Held) -> Option<u32> {
                thing.held().map(MTLThing::makeIt)
            }
            fn makeIt(one: u32) -> u32 {
                one
            }
            """
        )
        self.assertNotIn("makeIt", leg.calls_function.get("outer", set()))


class TestModulesAreNotTheLeg(unittest.TestCase):
    """A `#[cfg(test)]` module is the suite's code about the suite, not its Metal leg.

    Both directions matter and they fail differently. Read as the leg, a unit test's own receivers
    cost Metal selectors their credit — `case.name()` is a `Case`'s name, and 58 of the 130 sites
    that resolved to no item were one such module's calls. Worse, the call graph is keyed by
    function **name**, so a mock in a test module that spells a method of the real side merges the
    two: the real `run` carried the mock's callees, and every case reached through it was credited
    with items no case's Metal leg touches.
    """

    def test_a_call_inside_a_cfg_test_module_is_no_call_of_the_leg(self):
        """The same class message, inside a test module and outside one."""
        outside = leg_of(
            """
            fn made() -> u32 {
                MTLThing::makeIt(1)
            }
            """
        )
        self.assertEqual([call.rust for call in outside.calls], ["makeIt"])
        inside = leg_of(
            """
            #[cfg(test)]
            mod tests {
                fn made() -> u32 {
                    MTLThing::makeIt(1)
                }
            }
            """
        )
        self.assertEqual([call.rust for call in inside.calls], [])

    def test_a_mock_in_a_test_module_adds_no_edge_to_the_function_it_shadows(self):
        """A test module's `run` is not the side's `run`, so its callees are not `run`'s.

        The graph is keyed by name, so without this the walk from one `Side` method reaches
        everything every mock of it reaches. Held with the real function calling nothing, so the
        edge under test is the only one that could appear.
        """
        leg = leg_of(
            """
            fn run() -> u32 {
                0
            }
            fn probe() -> u32 {
                MTLThing::makeIt(1)
            }
            #[cfg(test)]
            mod tests {
                fn run() -> u32 {
                    probe()
                }
            }
            """
        )
        self.assertNotIn("probe", leg.calls_function.get("run", set()))

    def test_a_cfg_test_attribute_on_anything_else_leaves_the_code_alone(self):
        """`#[cfg(test)]` on a function is not a module, and its call is still the leg's.

        The narrowing this needs: the rule blanks a module, and a tree where it blanked whatever
        carried the attribute would read as a leg with no calls at all.
        """
        leg = leg_of(
            """
            #[cfg(test)]
            fn made() -> u32 {
                MTLThing::makeIt(1)
            }
            """
        )
        self.assertEqual([call.rust for call in leg.calls], ["makeIt"])

    def test_the_file_a_cfg_test_mod_declares_is_the_one_excluded(self):
        """`#[cfg(test)] mod tests;` names `tests.rs`, and a plain `mod` names nothing."""
        held = tempfile.TemporaryDirectory()
        MADE.append(held)
        root = Path(held.name)
        (root / "lib.rs").write_text(
            "#[cfg(test)]\nmod tests;\nmod helper;\npub mod metal_side;\n"
        )
        for name in ("tests.rs", "helper.rs", "metal_side.rs"):
            (root / name).write_text("")
        found = metal_leg.test_module_paths(root)
        self.assertIn(root / "tests.rs", found)
        self.assertNotIn(root / "helper.rs", found)
        self.assertNotIn(root / "metal_side.rs", found)


class HandSentSelectors(unittest.TestCase):
    """`msg_send![..]` is a call site, and the one the other loops cannot see.

    Every loop that reads a send needs a `(` after a name, and the macro has none, so a selector
    the crate binds no method for was in no table at all: not called, not unresolved, nowhere. One
    `MTLRasterizationRateMapDescriptor` constructor read as exercised by no case while the rate-map
    cases compared it against Metal.
    """

    def test_a_hand_sent_selector_resolves_to_the_item_its_receiver_binds(self):
        """The labels are the selector: `makeIt: .., with: ..` is `makeIt:with:`."""
        leg = leg_of(
            """
            fn send(thing: &ProtocolObject<dyn MTLThing>) {
                unsafe {
                    msg_send![
                        thing,
                        makeIt: 1,
                        with: 2,
                    ]
                }
            }
            """
        )
        self.assertEqual(
            [(call.rust, call.item) for call in leg.calls],
            [("makeIt_with", ("MTLThing", "makeIt:with:"))],
        )

    def test_a_message_to_a_class_object_resolves_against_that_class(self):
        """`T::class()` answers `T`, which no row of the map can say.

        This is the shape the rate-map constructor is sent in: the crate binds no method for the
        selector, so the leg takes the class object and sends it by hand.
        """
        leg = leg_of(
            """
            fn send() {
                let class = MTLThing::class();
                unsafe { msg_send![class, makeIt: 1] }
            }
            """
        )
        self.assertIn(("MTLThing", "makeIt:"), [call.item for call in leg.calls])

    def test_a_selector_objc2_binds_nowhere_is_its_own_pile(self):
        """An Apple SPI the crate has no name for: no container, so no item to credit."""
        leg = leg_of(
            """
            fn send(thing: &ProtocolObject<dyn MTLThing>) {
                unsafe { msg_send![thing, detachBacking] }
            }
            """
        )
        self.assertEqual([(f[2], f[3]) for f in leg.private], [("detachBacking", "detachBacking")])
        self.assertEqual(leg.unresolved, [])
        self.assertEqual(leg.foreign, [])

    def test_a_hand_sent_selector_its_receivers_type_lacks_is_unresolved(self):
        """A send the Rust types cannot express is not a typing error.

        The leg sends `arrayLength` to a `MTLBinding` it has just read as a texture binding,
        because `objc2` casts between two protocol objects nowhere. The receiver types to a
        supertype of the item's container, which `mistyped` must not hold: the generator fails on
        that pile, so counting this there would stop the document being written at all.
        """
        leg = leg_of(
            """
            fn send(thing: &ProtocolObject<dyn MTLThing>) {
                unsafe { msg_send![thing, special] }
            }
            """
        )
        self.assertEqual([(u[2], u[3]) for u in leg.unresolved], [("special", "thing")])
        self.assertEqual(leg.mistyped, [])

    def test_a_part_the_reading_cannot_name_is_no_call(self):
        """A macro body this cannot read is not a selector with a part dropped.

        A shortened name resolves to another item or to none, and either is worse than the hole:
        the document would say a case exercised something it never sent.
        """
        leg = leg_of(
            """
            fn send(thing: &ProtocolObject<dyn MTLThing>) {
                unsafe { msg_send![thing, makeIt: 1, 2] }
            }
            """
        )
        self.assertEqual([call.rust for call in leg.calls], [])


class Patterns(unittest.TestCase):
    """A binding is a pattern, and each slot of it takes its own type.

    `setLabel:` is the receiver of every call here, because both classes bind it: resolved, the
    typing did it, and a slot typed to the wrong class lands in `mistyped`, which each case also
    asserts is empty. 38 of the leg's unresolved sites were a name no `let` of this shape bound.
    """

    def items(self, source: str) -> list[tuple[str, str] | None]:
        leg = leg_of(source)
        self.assertEqual(leg.mistyped, [], "a slot typed to a class that does not bind the call")
        return [call.item for call in leg.calls if call.rust == "setLabel"]

    def test_a_tuple_pattern_takes_the_slots_of_a_tuple_expression(self):
        """`let (one, two) = (thing(), other())`, each slot from its own maker."""
        self.assertEqual(
            self.items(
                """
                fn thing() -> Retained<MTLThing> {
                    MTLThing::makeIt(1)
                }
                fn other() -> Retained<MTLOther> {
                    MTLOther::special()
                }
                fn send() {
                    let (one, two) = (thing(), other());
                    one.setLabel(1);
                    two.setLabel(2);
                }
                """
            ),
            [("MTLThing", "setLabel:"), ("MTLOther", "setLabel:")],
        )

    def test_a_tuple_pattern_takes_the_slots_of_a_declared_tuple_return(self):
        """`let (one, two) = pair()?`, from the tuple the function says it answers.

        Written over several lines with the trailing comma `rustfmt` puts there, which is the leg's
        own shape and the one that read as a tuple of three slots.
        """
        self.assertEqual(
            self.items(
                """
                fn pair() -> Answer<(
                    Retained<MTLThing>,
                    Retained<MTLOther>,
                )> {
                    todo!()
                }
                fn send() {
                    let (one, two) = pair()?;
                    one.setLabel(1);
                    two.setLabel(2);
                }
                """
            ),
            [("MTLThing", "setLabel:"), ("MTLOther", "setLabel:")],
        )

    def test_a_tuple_pattern_of_another_length_binds_nothing(self):
        """A slot this cannot name is left unbound, never bound to the whole."""
        leg = leg_of(
            """
            fn pair() -> (Retained<MTLThing>, Retained<MTLOther>) {
                todo!()
            }
            fn send() {
                let (one, two, three) = pair();
                one.setLabel(1);
            }
            """
        )
        self.assertEqual([call.item for call in leg.calls if call.rust == "setLabel"], [None])
        self.assertEqual(leg.mistyped, [])

    def test_a_for_pattern_takes_what_the_iterable_holds(self):
        """`for thing in &things` types each thing as the collection's element."""
        self.assertEqual(
            self.items(
                """
                fn send(things: Vec<Retained<MTLThing>>) {
                    for thing in &things {
                        thing.setLabel(1);
                    }
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_a_match_arm_takes_the_type_the_enum_declares_for_its_variant(self):
        """`Open::One(thing) =>` types `thing` from the enum, the only place that says it."""
        self.assertEqual(
            self.items(
                """
                enum Open {
                    None,
                    One(Retained<MTLThing>),
                }
                fn send(open: &Open) {
                    match open {
                        Open::None => {}
                        Open::One(thing) => thing.setLabel(1),
                    }
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_an_if_let_initializer_ends_at_the_block_it_guards(self):
        """`if let Some(thing) = self.held.take() {` has no `;` to end its initializer.

        Read to the `;` instead, the initializer swallowed the whole block, and the one receiver
        this source ever ends an encoder on typed as nothing.
        """
        self.assertEqual(
            self.items(
                """
                struct Side {
                    held: Option<Retained<MTLThing>>,
                }
                impl Side {
                    fn send(&mut self) {
                        if let Some(thing) = self.held.take() {
                            thing.setLabel(1);
                        }
                    }
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_an_initializer_is_not_cut_at_the_else_inside_a_name(self):
        """`.ok_or_else(..)?` ends an initializer; the `else` in its name does not.

        The leg turns every `nil` into a refusal with that adapter, so an `else` matched inside an
        identifier cut off the call that typed the receiver -- measured, 99 sites at once.
        """
        self.assertEqual(
            self.items(
                """
                fn made() -> Option<Retained<MTLThing>> {
                    todo!()
                }
                fn send() {
                    let thing = made().ok_or_else(|| 1)?;
                    thing.setLabel(1);
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )


class Receivers(unittest.TestCase):
    """What the text before a `.` is, and what it is not.

    The receiver is read backwards from its dot, and whitespace has to continue the walk because
    `rustfmt` breaks a chain across lines. So anything that can sit before a receiver and looks
    like part of an expression was read as part of it.
    """

    def items(self, source: str) -> list[tuple[str, str] | None]:
        leg = leg_of(source)
        self.assertEqual(leg.mistyped, [], "a receiver typed to a class that does not bind it")
        return [call.item for call in leg.calls if call.rust == "setLabel"]

    def test_a_keyword_before_the_receiver_is_not_part_of_it(self):
        """`match thing.setLabel(1) { .. }` sends to the thing, not to `match thing`."""
        self.assertEqual(
            self.items(
                """
                fn send(thing: &ProtocolObject<dyn MTLThing>) {
                    match thing
                        .setLabel(1)
                    {
                        _ => {}
                    }
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_a_path_before_the_receiver_is_part_of_it(self):
        """`MTLThing::class().setLabel(1)`: the `::` of a path continues the expression.

        Its pair is the `:` that does not -- a struct literal's field label -- and the two are one
        rule because the walk meets both as a colon.
        """
        self.assertEqual(
            self.items(
                """
                fn send() {
                    MTLThing::class().setLabel(1);
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_an_attribute_before_the_receiver_is_not_part_of_it(self):
        """`#[allow(deprecated)] thing.setLabel(1)`."""
        self.assertEqual(
            self.items(
                """
                fn send(thing: &ProtocolObject<dyn MTLThing>) {
                    #[allow(deprecated)]
                    thing.setLabel(1);
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_an_unsafe_block_is_a_receiver(self):
        """`unsafe { own.as_ref() }.setLabel(1)`, which is how a handler reads its own argument."""
        self.assertEqual(
            self.items(
                """
                fn send(own: NonNull<ProtocolObject<dyn MTLThing>>) {
                    unsafe { own.as_ref() }.setLabel(1);
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_any_other_block_is_not_a_receiver(self):
        """A `match`'s own block before a chained call is not what received the call.

        The narrowing the `unsafe` rule needs: a walk that read any block as a receiver would type
        the call against whatever the block happens to start with.
        """
        leg = leg_of(
            """
            fn send(thing: &ProtocolObject<dyn MTLThing>, other: &ProtocolObject<dyn MTLOther>) {
                match 1 {
                    _ => other.special(),
                }
                .setLabel(1);
            }
            """
        )
        self.assertEqual([call.item for call in leg.calls if call.rust == "setLabel"], [None])
        self.assertEqual(leg.mistyped, [])


class Expressions(unittest.TestCase):
    """What a chain answers, where the answer is not the last call's own."""

    def items(self, source: str) -> list[tuple[str, str] | None]:
        leg = leg_of(source)
        self.assertEqual(leg.mistyped, [], "a receiver typed to a class that does not bind it")
        return [call.item for call in leg.calls if call.rust == "setLabel"]

    def test_a_path_inside_an_argument_is_not_the_chains_own_separator(self):
        """`pool.get(i).ok_or_else(|| Failure::named(..))?` answers what the pool holds.

        Read as a path, the `::` of the refusal made the chain a message to a type called
        `Failure`, and every pool slot the leg reads this way typed as nothing.
        """
        self.assertEqual(
            self.items(
                """
                struct Side {
                    pool: Vec<Retained<MTLThing>>,
                }
                impl Side {
                    fn send(&self, at: usize) {
                        let held = self
                            .pool
                            .get(at)
                            .ok_or_else(|| Failure::named("NoSuchHandle", "gone"))?
                            .clone();
                        held.setLabel(1);
                    }
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_a_match_answers_the_first_of_its_arms_that_names_a_metal_object(self):
        """Every arm of a `match` answers one type, so the first that types is the type."""
        self.assertEqual(
            self.items(
                """
                fn made() -> Option<Retained<MTLThing>> {
                    todo!()
                }
                fn send(at: usize) {
                    let held = match at {
                        0 => return Err(Failure::named("No", "none")),
                        _ => made().ok_or_else(|| 1)?,
                    };
                    held.setLabel(1);
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_a_closure_called_as_a_value_answers_what_its_tail_answers(self):
        """`let make = || ..; make()?` is how the leg makes a second queue."""
        self.assertEqual(
            self.items(
                """
                fn made() -> Option<Retained<MTLThing>> {
                    todo!()
                }
                fn send() {
                    let make = || made().ok_or_else(|| 1);
                    let (one, two) = (make()?, make()?);
                    one.setLabel(1);
                    two.setLabel(2);
                }
                """
            ),
            [("MTLThing", "setLabel:"), ("MTLThing", "setLabel:")],
        )

    def test_a_closure_that_declares_what_it_answers_is_read_from_the_declaration(self):
        """A closure with a return type says it outright, as the leg's `named` helper does."""
        self.assertEqual(
            self.items(
                """
                fn send() {
                    let named = |at: usize| -> Answer<Retained<MTLThing>> {
                        todo!()
                    };
                    let held = named(0)?;
                    held.setLabel(1);
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )


class ClosureParameters(unittest.TestCase):
    """A closure parameter is typed by its declaration, or by the call that hands it a value."""

    def items(self, source: str) -> list[tuple[str, str] | None]:
        leg = leg_of(source)
        self.assertEqual(leg.mistyped, [], "a receiver typed to a class that does not bind it")
        return [call.item for call in leg.calls if call.rust == "setLabel"]

    def test_a_declared_parameter_survives_a_bitwise_or_above_it(self):
        """`A | B` must not open a parameter list that swallows the next closure's own `|`.

        Paired left to right, one bitwise or above a closure cost the declared type of eight
        receivers, every one of them a command buffer the leg fills a buffer through.
        """
        self.assertEqual(
            self.items(
                """
                fn send(mask: u32) {
                    let mask = MTLThing::READ | MTLThing::WRITE;
                    let fill = |held: &ProtocolObject<dyn MTLThing>| {
                        held.setLabel(1);
                    };
                    fill(&mask);
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_an_empty_parameter_list_opens_nothing(self):
        """`.ok_or_else(|| ..)` is a list of no parameters, and consuming it is the point."""
        self.assertEqual(
            self.items(
                """
                fn send() {
                    let made = || 1;
                    let fill = |held: &ProtocolObject<dyn MTLThing>| {
                        held.setLabel(1);
                    };
                    fill(&made());
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_an_undeclared_parameter_takes_the_element_of_what_it_was_called_on(self):
        """`vertex.map(|held| held.setLabel(..))`, where the type is on the receiver."""
        self.assertEqual(
            self.items(
                """
                fn send(thing: Option<Retained<MTLThing>>) {
                    thing.map(|held| held.setLabel(1));
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )

    def test_an_undeclared_parameter_of_an_error_combinator_is_not_the_element(self):
        """`ok_or_else` hands its closure the error, which names no Metal object.

        The narrowing: the element rule is for the combinators that pass an element, and reading
        an error as one would type a receiver against the class that was never there.
        """
        leg = leg_of(
            """
            fn send(thing: Option<Retained<MTLThing>>) {
                thing.ok_or_else(|held| held.setLabel(1));
            }
            """
        )
        self.assertEqual([call.item for call in leg.calls if call.rust == "setLabel"], [None])
        self.assertEqual(leg.mistyped, [])

    def test_an_undeclared_parameter_takes_what_the_leg_function_declares(self):
        """`self.encode(.., |encoder| ..)`: the type is in `encode`'s own signature."""
        self.assertEqual(
            self.items(
                """
                fn encode(work: impl FnOnce(&ProtocolObject<dyn MTLThing>)) {
                    todo!()
                }
                fn send() {
                    encode(|held| held.setLabel(1));
                }
                """
            ),
            [("MTLThing", "setLabel:")],
        )


if __name__ == "__main__":
    unittest.main()
