//! A case's shader, from a committed fixture.
//!
//! This is the primary path, not a fallback: a case names one of the owned fixtures under
//! `metal2vulkan_devtools::engine_dir()`, and the shader is assembled from its committed text at
//! run time. Nothing in the path needs `xcrun metal` or a prebuilt binary, which is what lets a
//! case run where there is no Metal — `scripts/linux-gate` streams the git-indexed files into the
//! container, so a committed fixture is there and a metallib built on a Mac is not.
//!
//! It is also where a recording's key comes from: the fixture's own bytes are what a case
//! fingerprints, so a recording decays loudly rather than silently when a fixture changes.
//!
//! Two texts, one shader. The runtime parses the bitcode `llvm-as` assembles from the fixture's
//! `.ll`; Metal's own assembler reads the fixture's `.air.ll` (typed pointers), which
//! `xcrun metal-as` reassembles and `xcrun metallib` packages. A case covers both in its input
//! fingerprint.
//!
//! # Which directory a name lies in
//!
//! The owned fixtures are not one directory: `fixtures/public/` holds `engine/` beside
//! `reflection/`, `msl_portability/` and the rest, each with its own `TOOLCHAIN` record and its own
//! reason to exist. A name with no `/` lies in `engine/`, which is where every case of this suite's
//! first rounds named its shader; a name with one `/` names its own directory under
//! `fixtures/public/`, so a case can read a fixture written for another lane rather than a copy of
//! it. The rule is the path's and not a case's: nothing here chooses a directory by what a fixture
//! holds, and a copied fixture would be a second set of bytes to keep in step with the first.

use crate::case::ShaderSource;
use metal2vulkan_devtools::{engine_dir, fixtures_dir, llvm_assemble, AssembleError};
use std::path::PathBuf;

/// The extension of the text the runtime's side assembles.
pub const RUNTIME_TEXT: &str = ".ll";

/// The extension of the text Metal's own assembler reads.
pub const METAL_TEXT: &str = ".air.ll";

/// A committed fixture, by its name without an extension.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fixture {
    /// The fixture's name, without an extension: bare for one of [`engine_dir`]'s, or
    /// `<directory>/<name>` for one of another directory's under `fixtures/public/`.
    pub name: &'static str,
    /// The entry point a case runs.
    pub entry: &'static str,
}

impl Fixture {
    /// The fixture `name`, whose entry point is its own name.
    pub const fn new(name: &'static str) -> Self {
        Self { name, entry: name }
    }

    /// The path of the fixture's companion in the dialect `extension` names.
    fn path(&self, extension: &str) -> PathBuf {
        let named = format!("{}{extension}", self.name);
        match self.name.contains('/') {
            true => fixtures_dir().join(named),
            false => engine_dir().join(named),
        }
    }

    /// The fixture's committed text in the dialect `extension` names ([`RUNTIME_TEXT`],
    /// [`METAL_TEXT`]).
    pub fn text(&self, extension: &str) -> Result<String, String> {
        let path = self.path(extension);
        std::fs::read_to_string(&path).map_err(|error| format!("{}: {error}", path.display()))
    }

    /// The AIR bitcode the runtime parses, as `llvm-as` assembles the fixture's `.ll`.
    pub fn bitcode(&self) -> Result<Vec<u8>, String> {
        llvm_assemble(&self.path(RUNTIME_TEXT)).map_err(|error| match error {
            AssembleError::Missing(why) => format!("no llvm-as: {why}"),
            AssembleError::Rejected(why) => format!("llvm-as rejected {}: {why}", self.name),
        })
    }

    /// Both forms of the fixture, held so a [`ShaderSource`] can borrow them.
    pub fn loaded(&self) -> Result<Loaded, String> {
        Ok(Loaded {
            bitcode: self.bitcode()?,
            air_text: self.text(METAL_TEXT)?,
            entry: self.entry,
        })
    }
}

/// A fixture's two forms in hand, from which a [`ShaderSource`] borrows.
pub struct Loaded {
    pub bitcode: Vec<u8>,
    pub air_text: String,
    pub entry: &'static str,
}

impl Loaded {
    /// The shader as a side takes it.
    pub fn source(&self) -> ShaderSource<'_> {
        ShaderSource {
            bitcode: &self.bitcode,
            air_text: &self.air_text,
            entry: self.entry,
        }
    }
}
