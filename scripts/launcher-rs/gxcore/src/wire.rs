use crate::error::GxError;

/// The signature is 86 base64url characters, always. gxlaunch.c:667.
pub const SIG_TEXT_LEN: usize = 86;

pub struct Split<'a> {
    pub payload: &'a str,
    pub signature: &'a str,
}

/// Separate the signed bytes from the signature.
///
/// The cut is the LAST '|', found by reverse search. Never by field index: the format grows by
/// appending payload fields, and an index-based cut would start slicing the signature off the
/// moment an eighth field appeared. gxlaunch.c:660-663.
pub fn split_wire(wire: &str) -> Result<Split<'_>, GxError> {
    let cut = wire.rfind('|').ok_or(GxError::NoSeparator)?;
    let payload = &wire[..cut];
    let signature = &wire[cut + 1..];
    if signature.len() != SIG_TEXT_LEN {
        return Err(GxError::BadSignatureLength(signature.len()));
    }
    Ok(Split { payload, signature })
}

#[cfg(test)]
mod tests {
    use super::*;

    const SIG86: &str = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

    #[test]
    fn the_fixture_signature_is_the_real_length() {
        assert_eq!(SIG86.len(), SIG_TEXT_LEN);
    }

    #[test]
    fn splits_on_the_last_pipe_not_the_first() {
        // A payload containing pipes is the whole point: the signature is found by
        // reverse search so that appending fields cannot dislodge it.
        let w = format!("2|123|0|abc|relay,,eu|base/|win32,n,h,1|{SIG86}");
        let s = split_wire(&w).unwrap();
        assert_eq!(s.signature, SIG86);
        assert_eq!(s.payload, "2|123|0|abc|relay,,eu|base/|win32,n,h,1");
    }

    #[test]
    fn a_document_with_no_pipe_is_refused() {
        assert!(matches!(split_wire("nopipe"), Err(GxError::NoSeparator)));
    }

    #[test]
    fn an_empty_signature_is_refused() {
        assert!(matches!(split_wire("2|x|"), Err(GxError::BadSignatureLength(0))));
    }
}
