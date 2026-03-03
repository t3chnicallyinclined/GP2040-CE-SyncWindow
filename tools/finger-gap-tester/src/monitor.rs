use gilrs::Button;
use std::collections::BTreeMap;
use std::time::Instant;

use crate::input::format_button;

/// Per-button tracking data.
struct ButtonState {
    /// Currently held down?
    held: bool,
    /// When the current/last press started.
    press_time: Option<Instant>,
    /// When the last release happened.
    release_time: Option<Instant>,
    /// Total press count.
    press_count: u32,
    /// Hold durations in ms.
    hold_durations: Vec<f64>,
    /// Time between consecutive presses of the same button (release→press) in ms.
    repress_gaps: Vec<f64>,
}

impl ButtonState {
    fn new() -> Self {
        Self {
            held: false,
            press_time: None,
            release_time: None,
            press_count: 0,
            hold_durations: Vec::new(),
            repress_gaps: Vec::new(),
        }
    }
}

pub struct ButtonMonitor {
    buttons: BTreeMap<ButtonKey, ButtonState>,
    event_log: Vec<MonitorLogEntry>,
}

/// Wrapper for Button that implements Ord for BTreeMap.
#[derive(Clone, Copy, PartialEq, Eq)]
struct ButtonKey(Button);

impl PartialOrd for ButtonKey {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for ButtonKey {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        format!("{:?}", self.0).cmp(&format!("{:?}", other.0))
    }
}

pub struct MonitorLogEntry {
    pub button_name: String,
    pub event_type: String,
    pub detail: String,
}

pub struct ButtonInfo {
    pub name: String,
    pub held: bool,
    pub press_count: u32,
    pub avg_hold_ms: f64,
    pub last_hold_ms: f64,
    pub avg_repress_ms: f64,
    pub last_repress_ms: f64,
}

impl ButtonMonitor {
    pub fn new() -> Self {
        Self {
            buttons: BTreeMap::new(),
            event_log: Vec::new(),
        }
    }

    pub fn on_press(&mut self, button: Button) {
        let key = ButtonKey(button);
        let state = self.buttons.entry(key).or_insert_with(ButtonState::new);
        let now = Instant::now();

        // Repress gap: time since last release of this button
        let repress_detail = if let Some(rel_time) = state.release_time {
            let gap = rel_time.elapsed().as_secs_f64() * 1000.0;
            state.repress_gaps.push(gap);
            format!("repress: {:.1}ms", gap)
        } else {
            String::new()
        };

        state.held = true;
        state.press_time = Some(now);
        state.press_count += 1;

        let detail = if repress_detail.is_empty() {
            format!("press #{}", state.press_count)
        } else {
            format!("press #{}  {}", state.press_count, repress_detail)
        };

        self.event_log.push(MonitorLogEntry {
            button_name: format_button(button),
            event_type: "PRESS".to_string(),
            detail,
        });

        if self.event_log.len() > 500 {
            self.event_log.remove(0);
        }
    }

    pub fn on_release(&mut self, button: Button) {
        let key = ButtonKey(button);
        let state = self.buttons.entry(key).or_insert_with(ButtonState::new);
        let now = Instant::now();

        // Hold duration: time since press
        let hold_detail = if let Some(press_time) = state.press_time {
            let dur = press_time.elapsed().as_secs_f64() * 1000.0;
            state.hold_durations.push(dur);
            format!("held: {:.1}ms", dur)
        } else {
            String::new()
        };

        state.held = false;
        state.release_time = Some(now);

        self.event_log.push(MonitorLogEntry {
            button_name: format_button(button),
            event_type: "RELEASE".to_string(),
            detail: hold_detail,
        });

        if self.event_log.len() > 500 {
            self.event_log.remove(0);
        }
    }

    /// Get info for all buttons that have been pressed at least once.
    pub fn button_infos(&self) -> Vec<ButtonInfo> {
        self.buttons
            .iter()
            .map(|(key, state)| {
                let avg_hold = if state.hold_durations.is_empty() {
                    0.0
                } else {
                    state.hold_durations.iter().sum::<f64>() / state.hold_durations.len() as f64
                };
                let last_hold = state.hold_durations.last().copied().unwrap_or(0.0);

                let avg_repress = if state.repress_gaps.is_empty() {
                    0.0
                } else {
                    state.repress_gaps.iter().sum::<f64>() / state.repress_gaps.len() as f64
                };
                let last_repress = state.repress_gaps.last().copied().unwrap_or(0.0);

                ButtonInfo {
                    name: format_button(key.0),
                    held: state.held,
                    press_count: state.press_count,
                    avg_hold_ms: avg_hold,
                    last_hold_ms: last_hold,
                    avg_repress_ms: avg_repress,
                    last_repress_ms: last_repress,
                }
            })
            .collect()
    }

    pub fn event_log(&self) -> &[MonitorLogEntry] {
        &self.event_log
    }

    pub fn clear(&mut self) {
        self.buttons.clear();
        self.event_log.clear();
    }
}
