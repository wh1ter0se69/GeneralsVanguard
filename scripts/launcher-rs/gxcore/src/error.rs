#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GxError {
    /// No '|' anywhere: not a manifest.
    NoSeparator,
    /// The signature text was not 86 base64url characters.
    BadSignatureLength(usize),
    /// The signature text did not decode to 64 bytes.
    BadSignatureEncoding,
    /// Ed25519 said no. The document is refused and the installed game runs.
    SignatureMismatch,
    /// Parsed cleanly but this build does not speak that version.
    UnsupportedVersion(i32),
    /// A required field was absent or unparseable.
    MalformedField(&'static str),
}
