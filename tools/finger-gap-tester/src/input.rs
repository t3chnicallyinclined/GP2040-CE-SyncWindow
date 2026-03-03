use gilrs::{Button, EventType, GamepadId, Gilrs};
use std::time::Instant;

const PAIR_WINDOW_MS: f64 = 50.0;

pub struct ButtonPair {
    pub button_a: Button,
    pub button_b: Button,
    pub gap_ms: f64,
}

#[derive(Clone)]
pub enum InputEvent {
    Pressed(Button),
    Released(Button),
}

struct PendingPress {
    button: Button,
    #[allow(dead_code)]
    gamepad_id: GamepadId,
    timestamp: Instant,
}

pub struct GamepadInput {
    gilrs: Gilrs,
    pending: Option<PendingPress>,
}

impl GamepadInput {
    pub fn new() -> Result<Self, gilrs::Error> {
        let gilrs = Gilrs::new()?;
        Ok(Self {
            gilrs,
            pending: None,
        })
    }

    /// Poll gilrs events. Processes only ONE ButtonPressed per call so that
    /// consecutive presses are naturally spaced across frames (~1ms apart via
    /// request_repaint_after). This gives real wall-clock timing between presses
    /// instead of draining the whole batch in nanoseconds.
    pub fn poll(&mut self) -> (Option<ButtonPair>, Vec<InputEvent>) {
        let mut pair = None;
        let mut events = Vec::new();

        while let Some(event) = self.gilrs.next_event() {
            match event.event {
                EventType::ButtonPressed(button, _) => {
                    events.push(InputEvent::Pressed(button));
                    let now = Instant::now();

                    match self.pending.take() {
                        None => {
                            self.pending = Some(PendingPress {
                                button,
                                gamepad_id: event.id,
                                timestamp: now,
                            });
                        }
                        Some(pending) => {
                            let gap_ms =
                                pending.timestamp.elapsed().as_secs_f64() * 1000.0;

                            if gap_ms <= PAIR_WINDOW_MS {
                                pair = Some(ButtonPair {
                                    button_a: pending.button,
                                    button_b: button,
                                    gap_ms,
                                });
                            } else {
                                self.pending = Some(PendingPress {
                                    button,
                                    gamepad_id: event.id,
                                    timestamp: now,
                                });
                            }
                        }
                    }
                    // Stop after one press — remaining events stay in gilrs
                    // buffer and get consumed next frame for accurate timing.
                    break;
                }
                EventType::ButtonReleased(button, _) => {
                    events.push(InputEvent::Released(button));
                }
                _ => {}
            }
        }

        // Expire stale pending press
        if let Some(ref pending) = self.pending {
            if pending.timestamp.elapsed().as_secs_f64() * 1000.0 > PAIR_WINDOW_MS {
                self.pending = None;
            }
        }

        (pair, events)
    }

    pub fn connected_gamepad_name(&self) -> Option<String> {
        self.gilrs
            .gamepads()
            .next()
            .map(|(_, gp)| gp.name().to_string())
    }

    #[allow(dead_code)]
    pub fn is_connected(&self) -> bool {
        self.gilrs.gamepads().next().is_some()
    }
}

pub fn format_button(button: Button) -> String {
    match button {
        Button::South => "A/Cross".to_string(),
        Button::East => "B/Circle".to_string(),
        Button::West => "X/Square".to_string(),
        Button::North => "Y/Triangle".to_string(),
        Button::LeftTrigger => "LB/L1".to_string(),
        Button::RightTrigger => "RB/R1".to_string(),
        Button::LeftTrigger2 => "LT/L2".to_string(),
        Button::RightTrigger2 => "RT/R2".to_string(),
        Button::LeftThumb => "LS".to_string(),
        Button::RightThumb => "RS".to_string(),
        Button::Select => "Back/Select".to_string(),
        Button::Start => "Start".to_string(),
        Button::DPadUp => "DPad Up".to_string(),
        Button::DPadDown => "DPad Down".to_string(),
        Button::DPadLeft => "DPad Left".to_string(),
        Button::DPadRight => "DPad Right".to_string(),
        other => format!("{:?}", other),
    }
}
