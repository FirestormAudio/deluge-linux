//! Put the front panel back to a known state between apps.

use deluge_linux_ui::{FRAME_BYTES, PAD_FRAME_BYTES};
use libdeluge::Deluge;

/// The parts of the instrument an app can leave in a visible or audible
/// state. Errors are ignored: a failed reset must never block the launcher.
pub trait Panel {
    fn leds_indicator(&mut self, id: i32, on: bool);
    fn leds_gold(&mut self, col: i32, i: i32, b: i32);
    fn leds_sync(&mut self, on: bool);
    fn pads_write(&mut self, buf: &[u8]);
    fn oled_write(&mut self, buf: &[u8]);
    fn cv_set_raw(&mut self, ch: i32, raw: u16);
    fn gate_set(&mut self, n: i32, on: bool);
}

impl Panel for Deluge {
    fn leds_indicator(&mut self, id: i32, on: bool) {
        let _ = Deluge::leds_indicator(self, id, on);
    }
    fn leds_gold(&mut self, col: i32, i: i32, b: i32) {
        let _ = Deluge::leds_gold(self, col, i, b);
    }
    fn leds_sync(&mut self, on: bool) {
        let _ = Deluge::leds_sync(self, on);
    }
    fn pads_write(&mut self, buf: &[u8]) {
        let _ = Deluge::pads_write(self, buf);
    }
    fn oled_write(&mut self, buf: &[u8]) {
        let _ = Deluge::oled_write(self, buf);
    }
    fn cv_set_raw(&mut self, ch: i32, raw: u16) {
        let _ = Deluge::cv_set_raw(self, ch, raw);
    }
    fn gate_set(&mut self, n: i32, on: bool) {
        let _ = Deluge::gate_set(self, n, on);
    }
}

/// All 36 indicators, both gold columns and SYNC off; pads and OLED blank;
/// both CV outputs at 0; all four gates off.
pub fn reset<P: Panel>(p: &mut P) {
    for id in 0..36 {
        p.leds_indicator(id, false);
    }
    for col in 0..2 {
        for i in 0..4 {
            p.leds_gold(col, i, 0);
        }
    }
    p.leds_sync(false);
    p.pads_write(&[0u8; PAD_FRAME_BYTES]);
    p.oled_write(&[0u8; FRAME_BYTES]);
    for ch in 0..2 {
        p.cv_set_raw(ch, 0);
    }
    for n in 0..4 {
        p.gate_set(n, false);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Rec {
        indicators_off: Vec<i32>,
        gold: Vec<(i32, i32, i32)>,
        sync_off: bool,
        pads: Vec<u8>,
        oled: Vec<u8>,
        cv: Vec<(i32, u16)>,
        gates_off: Vec<i32>,
    }

    impl Panel for Rec {
        fn leds_indicator(&mut self, id: i32, on: bool) {
            if !on {
                self.indicators_off.push(id);
            }
        }
        fn leds_gold(&mut self, col: i32, i: i32, b: i32) {
            self.gold.push((col, i, b));
        }
        fn leds_sync(&mut self, on: bool) {
            self.sync_off = !on;
        }
        fn pads_write(&mut self, buf: &[u8]) {
            self.pads = buf.to_vec();
        }
        fn oled_write(&mut self, buf: &[u8]) {
            self.oled = buf.to_vec();
        }
        fn cv_set_raw(&mut self, ch: i32, raw: u16) {
            self.cv.push((ch, raw));
        }
        fn gate_set(&mut self, n: i32, on: bool) {
            if !on {
                self.gates_off.push(n);
            }
        }
    }

    #[test]
    fn reset_clears_everything() {
        let mut r = Rec::default();
        reset(&mut r);
        assert_eq!(r.indicators_off, (0..36).collect::<Vec<_>>());
        assert_eq!(r.gold.len(), 8);
        assert!(r.gold.iter().all(|&(c, i, b)| (0..2).contains(&c) && (0..4).contains(&i) && b == 0));
        assert!(r.sync_off);
        assert_eq!(r.pads, vec![0u8; deluge_linux_ui::PAD_FRAME_BYTES]);
        assert_eq!(r.oled, vec![0u8; deluge_linux_ui::FRAME_BYTES]);
        assert_eq!(r.cv, vec![(0, 0), (1, 0)]);
        assert_eq!(r.gates_off, vec![0, 1, 2, 3]);
    }
}
