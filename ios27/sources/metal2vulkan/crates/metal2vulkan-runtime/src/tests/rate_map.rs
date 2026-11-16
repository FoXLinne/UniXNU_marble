//! The rasterization-rate map's creation rules and answers that need no device
//! (`rate_map::compile`, `rate_map::physical_size`, `rate_map::MapState`), and how far a pass over
//! the identity shades after a reset to another screen (`rate_mapped_pass::reset_bound`).

use super::super::error::{Error, Invalid, Problem};
use super::super::rate_map::{compile, physical_size, MapState, RateMapProblem};
use super::super::rate_mapped_pass::reset_bound;
use super::super::values::Size;
use metal2vulkan::bind::{
    RasterizationRateMap, RateMapDescriptor, RateMapLayerDescriptor, UnpinnedCompile,
};
use metal2vulkan::metal::Mutability;

/// A descriptor of `width` by `height` screen pixels and these layers' samples.
fn descriptor(width: u32, height: u32, layers: &[(&[f32], &[f32])]) -> RateMapDescriptor {
    let mut descriptor = RateMapDescriptor::new([width, height]);
    descriptor.layers = layers
        .iter()
        .map(|&(h, v)| RateMapLayerDescriptor::new(h.to_vec(), v.to_vec()))
        .collect();
    descriptor
}

/// `problem` at `new_rasterization_rate_map`.
fn refused(problem: RateMapProblem) -> Result<RasterizationRateMap, Error> {
    Err(Error::Invalid(Invalid::new(
        "new_rasterization_rate_map",
        Problem::RateMap(problem),
    )))
}

const HALF: (&[f32], &[f32]) = (&[0.5], &[0.5]);

/// A screen side past 32768, which Metal's validation stops, is refused; the screen's check comes
/// first. A side of 0, which its validation also stops, is not the runtime's refusal: plain Metal
/// compiles such a map, so the runtime compiles it too, with no physical extent on that axis.
#[test]
fn a_screen_side_is_at_most_32768_pixels() {
    for (width, height) in [(32769, 1), (1, 32769), (u32::MAX, 64), (0, 32769)] {
        assert_eq!(
            compile(&descriptor(width, height, &[HALF])),
            refused(RateMapProblem::ScreenSize { width, height }),
            "{width}x{height}"
        );
    }
    for (width, height) in [(1, 1), (64, 16384), (16384, 1)] {
        assert!(
            compile(&descriptor(width, height, &[HALF])).is_ok(),
            "{width}x{height}"
        );
    }
    // A screen side of 0, measured on this M3 Max under macOS 27.0.1 (plain Metal, one map per
    // process): the map is made, its screen size is the descriptor's and its physical extent on
    // that axis is 0.
    for (screen, physical) in [
        ([0, 64], [0, 33]),
        ([64, 0], [33, 0]),
        ([0, 0], [0, 0]),
        ([0, 256], [0, 132]),
    ] {
        let map = compile(&descriptor(screen[0], screen[1], &[HALF])).expect("the map is made");
        assert_eq!(map.screen_size(), screen, "{screen:?}");
        assert_eq!(map.physical_size(0), Some(physical), "{screen:?}");
        assert_eq!(
            physical_size(&map, 0),
            Size::new(u64::from(physical[0]), u64::from(physical[1]), 1),
            "{screen:?}"
        );
    }
    // An axis of no screen pixels takes any number of samples: no coordinate reads its quality
    // function, so the compile layer's density bound never applies to it, where the same four
    // samples over two screen pixels are `UnpinnedCompile::DenseSamples`
    // (`the_compile_layers_bounds_are_its_own_error`).
    let dense: &[f32] = &[0.5, 0.25, 0.5, 0.25];
    for samples in [&[0.5, 1.0][..], dense] {
        let map = compile(&descriptor(0, 64, &[(samples, &[0.5])])).expect("the map is made");
        assert_eq!(map.physical_size(0), Some([0, 33]), "{samples:?}");
    }
    // A map of no layers over a screen of no width is made as well, as plain Metal makes it.
    let none = compile(&descriptor(0, 64, &[])).expect("the map is made");
    assert_eq!(none.layer_count(), 0);
    assert_eq!(physical_size(&none, 0), Size::new(0, 16, 1));
}

/// A map takes 0 to 2 layers, counted as Metal counts them: from index 0 up to the first layer
/// without samples on an axis (nil in Metal's descriptor). The layers after it are ignored. A count
/// of 0 makes a map of no layers, as plain Metal does.
#[test]
fn a_map_takes_zero_to_two_layers() {
    let empty = compile(&descriptor(64, 64, &[])).unwrap();
    assert_eq!(empty.layer_count(), 0);
    assert_eq!(
        compile(&descriptor(64, 64, &[HALF; 3])),
        refused(RateMapProblem::LayerCount { count: 3 })
    );
    let none: &[f32] = &[];
    let quarter: (&[f32], &[f32]) = (&[0.25], &[0.25]);
    for layers in [&[(none, &[0.5][..]), quarter][..], &[(&[0.5][..], none)]] {
        assert_eq!(compile(&descriptor(64, 64, layers)), Ok(empty.clone()));
    }
    let first = compile(&descriptor(64, 64, &[HALF])).unwrap();
    for layers in [
        &[HALF, (none, &[0.5][..])][..],
        &[HALF, (&[0.5][..], none), quarter],
    ] {
        assert_eq!(compile(&descriptor(64, 64, layers)), Ok(first.clone()));
    }
    let two = compile(&descriptor(64, 64, &[HALF, quarter])).unwrap();
    assert_eq!(
        compile(&descriptor(64, 64, &[HALF, quarter, (none, none), HALF])),
        Ok(two)
    );
}

/// Past Metal's validation, the compile layer's bounds are `Error::RateMap`: a screen past 16384 a
/// side, and samples whose compile is not measured.
#[test]
fn the_compile_layers_bounds_are_its_own_error() {
    for (width, height, extent) in [(16385, 64, 16385), (64, 32768, 32768)] {
        assert_eq!(
            compile(&descriptor(width, height, &[HALF])),
            Err(Error::RateMap(UnpinnedCompile::ScreenExtent { extent })),
            "{width}x{height}"
        );
    }
    let dense: &[f32] = &[0.5, 0.25, 0.5, 0.25];
    assert!(matches!(
        compile(&descriptor(2, 64, &[(dense, &[0.5])])),
        Err(Error::RateMap(UnpinnedCompile::DenseSamples { .. }))
    ));
}

/// `physicalSizeForLayer:` is the layer's physical size, depth 1, and past the layers width 0
/// and the layers' shared vertical extent: Metal's height there, and its width just past a map
/// of one layer.
#[test]
fn a_layer_past_the_map_has_no_width_and_the_shared_height() {
    let map = compile(&descriptor(64, 64, &[HALF, (&[1.0], &[0.25])])).unwrap();
    assert_eq!(physical_size(&map, 0), Size::new(33, 33, 1));
    assert_eq!(physical_size(&map, 1), Size::new(64, 33, 1));
    for past in [2, u64::from(u32::MAX), u64::MAX] {
        assert_eq!(physical_size(&map, past), Size::new(0, 33, 1), "{past}");
    }
    // A map of no layers: width 0 and its vertical axis's 16 rows at layers 0 and 1, as Metal
    // answers (an M4); at layer 2 Metal reads 16 past its table, the runtime 0.
    let empty = compile(&descriptor(256, 256, &[])).unwrap();
    for layer in [0, 1, 2] {
        assert_eq!(physical_size(&empty, layer), Size::new(0, 16, 1), "{layer}");
    }
}

/// A mutable map before its first reset answers as Metal's (our probe `rm`, the M3 Max under
/// macOS 27.0.1): its descriptor's screen and layer count, a physical size of 0 by 0 at every
/// layer, and parameter data of zeros. Its reset compiles the descriptor given as a fresh map of
/// it would be, and a map of another mutability is compiled at once.
#[test]
fn a_mutable_map_is_compiled_at_its_reset() {
    let mut mutable = descriptor(256, 128, &[(&[0.1], &[0.1])]);
    mutable.mutability = Mutability::Mutable;
    let unset = MapState::made(&mutable, false).unwrap();
    assert!(unset.compiled().is_none());
    assert_eq!(unset.screen_size(), [256, 128]);
    assert_eq!(unset.layer_count(), 1);
    for layer in [0, 1, 2, u64::MAX] {
        assert_eq!(unset.physical_size(layer), Size::new(0, 0, 1), "{layer}");
    }
    assert!(unset.words().iter().all(|&word| word == 0));
    assert_eq!(unset.words().len(), 24_624 / 4);
    let plain = descriptor(256, 128, &[(&[0.1], &[0.1])]);
    let reset = unset.reset(&plain, false).unwrap();
    let fresh = MapState::made(&plain, false).unwrap();
    assert_eq!(reset.compiled(), fresh.compiled());
    assert_eq!(reset.physical_size(0), Size::new(32, 16, 1));
    // Plain Metal makes a mutable map of a descriptor its API validation stops.
    for (screen, layers) in [([64, 64], 3), ([0, 64], 1), ([40000, 64], 1)] {
        let mut stopped = descriptor(screen[0], screen[1], &[HALF; 3][..layers]);
        stopped.mutability = Mutability::Mutable;
        let unset = MapState::made(&stopped, false).unwrap();
        assert_eq!(unset.screen_size(), screen);
        assert_eq!(unset.layer_count(), layers as u32);
        assert_eq!(unset.physical_size(0), Size::new(0, 0, 1));
        let reset = unset.reset(&plain, false).unwrap();
        assert_eq!(reset.compiled(), fresh.compiled());
    }
    for mutability in [Mutability::Default, Mutability::Immutable] {
        let mut d = plain.clone();
        d.mutability = mutability;
        assert_eq!(
            MapState::made(&d, false).unwrap().compiled(),
            fresh.compiled(),
            "{mutability:?}"
        );
    }
}

/// A reset refuses what ends Metal's process, three layers, and what the compile layer does not
/// compile (Metal's validation does not look at a reset), leaving nothing changed. A reset to a
/// screen side of 0 compiles, as plain Metal compiles it.
#[test]
fn a_reset_refuses_what_metal_or_the_compile_stops() {
    let map = MapState::made(&descriptor(64, 64, &[HALF]), false).unwrap();
    assert_eq!(
        map.reset(&descriptor(64, 64, &[HALF; 3]), false).err(),
        Some(Error::Invalid(Invalid::new(
            "reset_using_descriptor",
            Problem::RateMap(RateMapProblem::LayerCount { count: 3 }),
        )))
    );
    for (width, height, extent) in [(16385, 64, 16385), (70000, 64, 70000)] {
        assert_eq!(
            map.reset(&descriptor(width, height, &[HALF]), false).err(),
            Some(Error::RateMap(UnpinnedCompile::ScreenExtent { extent })),
            "{width}x{height}"
        );
    }
    // A reset to a screen side of 0 compiles, as a fresh map of one does (`apple_tests` holds
    // Metal's words for the reset both ways).
    for screen in [[0, 64], [64, 0], [0, 0]] {
        let reset = map
            .reset(&descriptor(screen[0], screen[1], &[HALF]), false)
            .expect("the reset compiles");
        assert_eq!(reset.screen_size(), screen, "{screen:?}");
    }
}

/// A map reset to fewer layers keeps the physical width each layer below 2 held, at the reset's
/// vertical extent, as Metal answers (our probe `none`, the M3 Max under macOS 27.0.1: maps of
/// 100 by 60 screen pixels, one layer of quality 1 and of 0.5, and two of 0.5 and 0.25, reset to
/// no layers over 64 by 32). Past layer 1 Metal reads past its table (16 at layer 2); the
/// runtime answers width 0 there. A map reset to none after layers draws from what they left.
#[test]
fn a_reset_keeps_the_widths_past_its_layers() {
    let none = descriptor(64, 32, &[]);
    for (layers, widths) in [
        (&[(&[1.0][..], &[1.0][..])][..], [100, 0]),
        (&[HALF], [64, 0]),
        (&[HALF, (&[0.25], &[0.25])], [64, 32]),
    ] {
        let earlier = MapState::made(&descriptor(100, 60, layers), false).unwrap();
        let reset = earlier.reset(&none, false).unwrap();
        assert_eq!(reset.layer_count(), 0);
        for (layer, width) in widths.into_iter().enumerate() {
            assert_eq!(
                reset.physical_size(layer as u64),
                Size::new(width, 16, 1),
                "{widths:?} at {layer}"
            );
        }
        assert_eq!(reset.physical_size(2), Size::new(0, 16, 1));
        assert!(reset.made_after_release());
    }
    let fresh = MapState::made(&none, false).unwrap();
    assert!(!fresh.made_after_release());
    assert!(MapState::made(&none, true).unwrap().made_after_release());
    assert_eq!(fresh.physical_size(0), Size::new(0, 16, 1));
}

/// A pass over the identity, its map reset after its encoder took it from a square screen of
/// `taken` pixels to one of `now`, shades as far as Metal's did on each axis: every case of our
/// probe `rs` (the M3 Max under macOS 27.0.1, the reset made after the encoder, after its end and
/// after the commit of a waiting command buffer alike), `(taken, now, shaded)`.
#[test]
fn a_reset_to_another_screen_shades_as_far_as_metal_s_pass() {
    const MEASURED: &[(u32, u32, u32)] = &[
        (8, 16, 8),
        (8, 40, 8),
        (16, 8, 16),
        (24, 48, 24),
        (32, 64, 32),
        (33, 8, 32),
        (33, 32, 32),
        (40, 8, 32),
        (48, 24, 32),
        (50, 100, 50),
        (64, 32, 32),
        (64, 40, 64),
        (64, 63, 64),
        (65, 40, 64),
        (65, 64, 64),
        (96, 16, 96),
        (96, 33, 64),
        (96, 60, 64),
        (96, 64, 64),
        (100, 50, 100),
        (100, 80, 96),
        (128, 8, 128),
        (128, 16, 128),
        (128, 24, 128),
        (128, 31, 128),
        (128, 32, 128),
        (128, 33, 128),
        (128, 40, 128),
        (128, 48, 128),
        (128, 50, 128),
        (128, 56, 128),
        (128, 63, 128),
        (128, 64, 128),
        (128, 65, 96),
        (128, 80, 96),
        (128, 96, 96),
        (128, 100, 128),
        (128, 127, 128),
        (160, 70, 160),
        (160, 100, 128),
        (160, 130, 160),
        (200, 8, 200),
        (200, 32, 200),
        (200, 33, 200),
        (200, 50, 200),
        (200, 64, 200),
        (200, 65, 200),
        (200, 100, 200),
        (200, 128, 200),
        (200, 129, 200),
        (200, 150, 200),
        (200, 160, 200),
        (200, 161, 192),
        (200, 170, 192),
        (200, 192, 192),
        (256, 100, 256),
        (256, 200, 224),
    ];
    for &(taken, now, shaded) in MEASURED {
        assert_eq!(reset_bound(taken, now), shaded, "{taken} reset to {now}");
    }
    // A reset to the screen taken changes nothing.
    for side in [1, 31, 32, 33, 64, 16384] {
        assert_eq!(reset_bound(side, side), side);
    }
}
