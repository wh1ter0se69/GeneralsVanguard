//! Times the Keybinds cameo path on the real install: `cargo run --release -p gxio --example cameo_timing`.
//! Cold = first palette (atlases decode), warm = the same palette again, switch = the next faction's palette.
use gxio::commandset::CommandData;
use std::time::Instant;

fn main() {
    let g = gxio::game::discover();
    let t = Instant::now();
    let d = CommandData::load(g.data_dir.as_deref(), g.patch_dir.as_deref()).expect("load");
    println!("load (archive index + INI parse): {:?}", t.elapsed());
    let mut sets: Vec<(String, Vec<String>)> = Vec::new();
    for gen in d.generals.iter().filter(|g| g.is_base) {
        let mut names: Vec<String> = Vec::new();
        for s in &gen.structures {
            names.extend(s.cameo.clone());
            names.extend(s.buttons.iter().filter_map(|b| b.cameo.clone()));
        }
        names.sort();
        names.dedup();
        sets.push((gen.name.clone(), names));
    }
    if let Ok(path) = std::env::var("CAMEO_DUMP") {
        // The first faction's palette as the screen receives it (name -> data URL), for measuring the webview side.
        let mut m = std::collections::BTreeMap::new();
        for n in &sets[0].1 {
            if let Ok(u) = d.cameo_data_url(n, 120, 96) { m.insert(n.clone(), u); }
        }
        let bytes: usize = m.values().map(|u| u.len()).sum();
        println!("palette payload: {} cameos, {} bytes of data URL", m.len(), bytes);
        let mut j = String::from("{");
        for (i, (k, v)) in m.iter().enumerate() { if i > 0 { j.push(','); } j.push_str(&format!("\"{}\":\"{}\"", k, v)); }
        j.push('}');
        std::fs::write(path, j).unwrap();
    }
    for (label, pass) in [("cold", 0), ("warm", 1)] {
        for (name, names) in &sets {
            let t = Instant::now();
            let ok = names.iter().filter(|n| d.cameo_data_url(n, 120, 96).is_ok()).count();
            println!("{label} {name}: {} cameos ({} ok) in {:?}", names.len(), ok, t.elapsed());
        }
        let _ = pass;
    }
}
