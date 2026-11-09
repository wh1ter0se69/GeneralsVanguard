mod error;
#[cfg(test)]
mod fixtures;
mod manifest;
mod plan;
mod state;
mod verify;
mod wire;

pub use error::GxError;
pub use manifest::{
    artifact_hash, hash_matches, parse, supersedes, Artifact, ArtifactHasher, Manifest, Relay,
    MANIFEST_VERSION, MAX_ARTIFACTS, MAX_RELAYS,
};
pub use plan::{decide, Verdict};
pub use state::{InstallState, InstalledItem};
pub use verify::{verify, verify_with, Verified, GX_MANIFEST_KEYS, GX_MANIFEST_PUBKEY};
pub use wire::{split_wire, Split, SIG_TEXT_LEN};
