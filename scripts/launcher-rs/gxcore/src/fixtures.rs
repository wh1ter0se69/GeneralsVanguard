//! GeneralsX @test Claude 29/10/2026 S1-3: the SHARED relay-list fixtures (scripts/test/relay-list-fixtures.py),
//! signed by OpenSSL with public TEST keys. The engine and the C launcher test the same bytes.
macro_rules! fx {
    ($name:ident, $file:literal) => {
        pub const $name: &str = include_str!(concat!("../../../test/relay-list-fixtures/", $file));
    };
}
fx!(K1_HOME, "k1-home.txt");
fx!(K1_OLDER, "k1-older.txt");
fx!(K1_RESIGN_LOWER, "k1-resign-lower.txt");
fx!(K1_RESIGN_HIGHER, "k1-resign-higher.txt");
fx!(K1_FUTURE, "k1-future.txt");
fx!(K1_AFTER_K2, "k1-after-k2.txt");
fx!(K1_LEGACY7, "k1-legacy7.txt");
fx!(K1_EXTRA_FIELD, "k1-extra-field.txt");
fx!(K1_PORT, "k1-port.txt");
fx!(K2_HOME, "k2-home.txt");
fx!(K2_RECOVERY_OLDORD, "k2-recovery-oldord.txt");
fx!(K3_HOME, "k3-home.txt");
fx!(ROGUE_HOME, "rogue-home.txt");
