//! Prova de uma denúncia: um quadro da transmissão (arquivo de quadros do core) reduzido e salvo como JPEG de no máximo 480 KB.
use std::path::Path;

const MAX_W: u32 = 960;
const MAX_BYTES: usize = 480 * 1024;

/// RGBA (w x h) -> JPEG: reduz para no máximo 960 px de largura e baixa a qualidade até caber em 480 KB.
pub fn jpeg_from_rgba(w: usize, h: usize, rgba: &[u8]) -> Option<Vec<u8>> {
    if w == 0 || h == 0 || rgba.len() < w * h * 4 { return None; }
    let img = image::RgbaImage::from_raw(w as u32, h as u32, rgba[..w * h * 4].to_vec())?;
    let img = image::DynamicImage::ImageRgba8(img);
    let img = if img.width() > MAX_W { img.resize(MAX_W, (img.height() as u64 * MAX_W as u64 / img.width() as u64).max(1) as u32, image::imageops::FilterType::Triangle) } else { img };
    let rgb = img.to_rgb8();
    for q in [75u8, 60, 45, 30] {
        let mut out = Vec::new();
        if image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, q).encode_image(&rgb).is_err() { return None; }
        if out.len() <= MAX_BYTES { return Some(out); }
    }
    None
}

/// Lê o último quadro do arquivo de quadros de uma transmissão (o mesmo que a UI exibe) e devolve o JPEG; None se não há quadro.
pub fn jpeg_from_frame_file(path: &Path) -> Option<Vec<u8>> {
    let src = crate::video::FrameSource::open(path).ok()?;
    let (w, h, _n, rgba) = src.read()?;
    jpeg_from_rgba(w, h, &rgba)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rgba_becomes_a_small_jpeg() {
        let (w, h) = (1920usize, 1080usize);
        let mut px = vec![0u8; w * h * 4];
        for (i, c) in px.chunks_exact_mut(4).enumerate() { c[0] = (i % 251) as u8; c[1] = ((i / w) % 255) as u8; c[2] = 90; c[3] = 255; }
        let j = jpeg_from_rgba(w, h, &px).expect("jpeg");
        assert!(j.len() <= MAX_BYTES && j.starts_with(&[0xff, 0xd8]), "tamanho {}", j.len());
        let back = image::load_from_memory(&j).unwrap();
        assert_eq!(back.width(), MAX_W);
        assert!(jpeg_from_rgba(0, 0, &[]).is_none() && jpeg_from_rgba(10, 10, &[0; 8]).is_none());
    }
}
