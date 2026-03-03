use eframe::egui;
use egui::{Color32, RichText, ScrollArea, Ui};
use egui_plot::{Bar, BarChart, Plot};

use crate::input::{format_button, GamepadInput, InputEvent};
use crate::monitor::ButtonMonitor;
use crate::stats::GapStats;

const TEAL: Color32 = Color32::from_rgb(0, 180, 216);
const GREEN: Color32 = Color32::from_rgb(80, 200, 80);
const YELLOW: Color32 = Color32::from_rgb(220, 180, 40);
const RED: Color32 = Color32::from_rgb(220, 60, 60);
const LOG_MAX: usize = 500;

#[derive(PartialEq)]
enum Tab {
    GapTester,
    ButtonMonitor,
}

struct GapLogEntry {
    attempt: usize,
    button_a: String,
    button_b: String,
    gap_ms: f64,
    running_avg: f64,
}

pub struct FingerGapApp {
    input: Option<GamepadInput>,
    stats: GapStats,
    gap_log: Vec<GapLogEntry>,
    monitor: ButtonMonitor,
    active_tab: Tab,
    error_msg: Option<String>,
}

impl FingerGapApp {
    pub fn new() -> Self {
        let (input, error_msg) = match GamepadInput::new() {
            Ok(gi) => (Some(gi), None),
            Err(e) => (None, Some(format!("Gamepad init failed: {e}"))),
        };
        Self {
            input,
            stats: GapStats::new(),
            gap_log: Vec::new(),
            monitor: ButtonMonitor::new(),
            active_tab: Tab::GapTester,
            error_msg,
        }
    }
}

impl eframe::App for FingerGapApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // Poll gamepad - get pair detection + raw events
        if let Some(ref mut input) = self.input {
            let (pair, events) = input.poll();

            // Feed raw events to button monitor
            for ev in &events {
                match ev {
                    InputEvent::Pressed(btn) => self.monitor.on_press(*btn),
                    InputEvent::Released(btn) => self.monitor.on_release(*btn),
                }
            }

            // Record gap pair
            if let Some(pair) = pair {
                self.stats.record(pair.gap_ms);
                let avg = self.stats.average();
                self.gap_log.push(GapLogEntry {
                    attempt: self.stats.count(),
                    button_a: format_button(pair.button_a),
                    button_b: format_button(pair.button_b),
                    gap_ms: pair.gap_ms,
                    running_avg: avg,
                });
                if self.gap_log.len() > LOG_MAX {
                    self.gap_log.remove(0);
                }
            }
        }

        ctx.request_repaint_after(std::time::Duration::from_millis(1));

        // === TOP BAR ===
        egui::TopBottomPanel::top("header").show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.heading(RichText::new("NOBD INPUT TESTER").strong());
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if ui.button("Reset").clicked() {
                        self.stats.clear();
                        self.gap_log.clear();
                        self.monitor.clear();
                    }
                });
            });

            // Controller status
            if let Some(ref err) = self.error_msg {
                ui.colored_label(Color32::RED, format!("Error: {err}"));
            } else if let Some(ref input) = self.input {
                if let Some(name) = input.connected_gamepad_name() {
                    ui.horizontal(|ui| {
                        ui.colored_label(GREEN, "\u{25CF}");
                        ui.label(format!("Controller: {name}"));
                    });
                } else {
                    ui.horizontal(|ui| {
                        ui.colored_label(YELLOW, "\u{25CF}");
                        ui.label("No controller detected. Connect a gamepad.");
                    });
                }
            }

            ui.separator();

            // Tabs
            ui.horizontal(|ui| {
                ui.selectable_value(
                    &mut self.active_tab,
                    Tab::GapTester,
                    RichText::new("  Gap Tester  ").size(15.0),
                );
                ui.selectable_value(
                    &mut self.active_tab,
                    Tab::ButtonMonitor,
                    RichText::new("  Button Monitor  ").size(15.0),
                );
            });
        });

        match self.active_tab {
            Tab::GapTester => draw_gap_tester(ctx, &self.stats, &self.gap_log),
            Tab::ButtonMonitor => draw_button_monitor(ctx, &self.monitor),
        }
    }
}

// ─── GAP TESTER TAB ───

fn draw_gap_tester(ctx: &egui::Context, stats: &GapStats, log: &[GapLogEntry]) {
    egui::TopBottomPanel::bottom("gap_log")
        .min_height(120.0)
        .resizable(true)
        .show(ctx, |ui| {
            ui.heading("Attempt Log");
            ui.separator();
            ScrollArea::vertical()
                .stick_to_bottom(true)
                .show(ui, |ui| {
                    for entry in log.iter().rev() {
                        ui.monospace(format!(
                            "#{:>3}  {} + {}  gap: {:5.1}ms  (avg: {:.1}ms)",
                            entry.attempt,
                            entry.button_a,
                            entry.button_b,
                            entry.gap_ms,
                            entry.running_avg,
                        ));
                    }
                });
        });

    egui::CentralPanel::default().show(ctx, |ui| {
        if stats.count() > 0 {
            ui.add_space(8.0);
            egui::Frame::new()
                .inner_margin(12.0)
                .corner_radius(8.0)
                .stroke(egui::Stroke::new(2.0, TEAL))
                .show(ui, |ui| {
                    ui.vertical_centered(|ui| {
                        ui.label(
                            RichText::new("RECOMMENDED NOBD VALUE")
                                .size(14.0)
                                .color(Color32::GRAY),
                        );
                        ui.label(
                            RichText::new(format!("{} ms", stats.recommended_nobd()))
                                .size(48.0)
                                .strong()
                                .color(TEAL),
                        );
                        ui.label(
                            RichText::new(format!(
                                "covers your slowest gap of {:.1}ms + 2ms headroom",
                                stats.max()
                            ))
                            .size(12.0)
                            .color(Color32::GRAY),
                        );
                    });
                });
            ui.add_space(8.0);
        } else {
            ui.add_space(20.0);
            ui.vertical_centered(|ui| {
                ui.label(
                    RichText::new("Press two buttons at the same time to start measuring")
                        .size(16.0)
                        .color(Color32::GRAY),
                );
                ui.add_space(4.0);
                ui.label(
                    RichText::new("(like LP+HP for a dash)")
                        .size(13.0)
                        .color(Color32::DARK_GRAY),
                );
            });
            ui.add_space(20.0);
        }

        ui.separator();

        let available = ui.available_size();
        ui.horizontal_top(|ui| {
            // Left: Live stats
            ui.vertical(|ui| {
                ui.set_min_width(160.0);
                ui.heading("Live Stats");
                ui.add_space(8.0);
                draw_stat(ui, "Attempts", &format!("{}", stats.count()));
                if stats.count() > 0 {
                    draw_stat(ui, "Average", &format!("{:.1}ms", stats.average()));
                    draw_stat(ui, "Median", &format!("{:.1}ms", stats.median()));
                    draw_stat(ui, "Fastest", &format!("{:.1}ms", stats.min()));
                    draw_stat(ui, "Slowest", &format!("{:.1}ms", stats.max()));
                }
            });

            ui.separator();

            // Right: Histogram
            ui.vertical(|ui| {
                ui.heading("Distribution");
                ui.add_space(4.0);

                let buckets = stats.histogram_buckets();
                if buckets.is_empty() {
                    ui.colored_label(Color32::DARK_GRAY, "No data yet");
                } else {
                    let bars: Vec<Bar> = buckets
                        .iter()
                        .enumerate()
                        .filter(|(_, (_, count, _))| *count > 0)
                        .map(|(i, (_label, count, _pct))| {
                            Bar::new(i as f64, *count as f64)
                                .width(0.7)
                                .fill(TEAL)
                        })
                        .collect();

                    let labels: Vec<(usize, String)> = buckets
                        .iter()
                        .enumerate()
                        .map(|(i, (label, _, _))| (i, label.clone()))
                        .collect();

                    let chart_height = (available.y * 0.45).max(120.0).min(250.0);

                    Plot::new("gap_histogram")
                        .height(chart_height)
                        .allow_drag(false)
                        .allow_zoom(false)
                        .allow_scroll(false)
                        .allow_boxed_zoom(false)
                        .show_axes([true, true])
                        .x_axis_formatter(move |val, _range| {
                            let idx = val.value.round() as usize;
                            labels
                                .iter()
                                .find(|(i, _)| *i == idx)
                                .map(|(_, l)| l.clone())
                                .unwrap_or_default()
                        })
                        .y_axis_formatter(|val, _range| format!("{}", val.value as u32))
                        .show(ui, |plot_ui| {
                            plot_ui.bar_chart(BarChart::new("gaps".to_string(), bars));
                        });
                }
            });
        });
    });
}

// ─── BUTTON MONITOR TAB ───

fn draw_button_monitor(ctx: &egui::Context, monitor: &ButtonMonitor) {
    egui::TopBottomPanel::bottom("monitor_log")
        .min_height(150.0)
        .resizable(true)
        .show(ctx, |ui| {
            ui.heading("Event Log");
            ui.separator();
            ScrollArea::vertical()
                .stick_to_bottom(true)
                .show(ui, |ui| {
                    for entry in monitor.event_log().iter().rev() {
                        ui.horizontal(|ui| {
                            let color = if entry.event_type == "PRESS" {
                                GREEN
                            } else {
                                Color32::GRAY
                            };
                            ui.monospace(
                                RichText::new(format!(
                                    "{:<14} {:<8} {}",
                                    entry.button_name, entry.event_type, entry.detail,
                                ))
                                .color(color),
                            );
                        });
                    }
                });
        });

    egui::CentralPanel::default().show(ctx, |ui| {
        let infos = monitor.button_infos();

        if infos.is_empty() {
            ui.add_space(40.0);
            ui.vertical_centered(|ui| {
                ui.label(
                    RichText::new("Press any button to start monitoring")
                        .size(16.0)
                        .color(Color32::GRAY),
                );
                ui.add_space(4.0);
                ui.label(
                    RichText::new("Shows hold duration, repress timing, and activation stats")
                        .size(13.0)
                        .color(Color32::DARK_GRAY),
                );
            });
            return;
        }

        // Live button states
        ui.add_space(8.0);
        ui.heading("Active Buttons");
        ui.add_space(4.0);
        ui.horizontal_wrapped(|ui| {
            for info in &infos {
                let (color, text_color) = if info.held {
                    (TEAL, Color32::BLACK)
                } else {
                    (Color32::from_rgb(40, 40, 50), Color32::GRAY)
                };
                egui::Frame::new()
                    .inner_margin(egui::vec2(12.0, 6.0))
                    .corner_radius(4.0)
                    .fill(color)
                    .show(ui, |ui| {
                        ui.label(RichText::new(&info.name).strong().color(text_color));
                    });
            }
        });

        ui.add_space(12.0);
        ui.separator();
        ui.add_space(8.0);

        // Per-button stats table
        ui.heading("Button Stats");
        ui.add_space(4.0);

        egui::Grid::new("button_stats")
            .striped(true)
            .min_col_width(80.0)
            .show(ui, |ui| {
                // Header
                ui.label(RichText::new("Button").strong().color(TEAL));
                ui.label(RichText::new("Presses").strong().color(TEAL));
                ui.label(RichText::new("Last Hold").strong().color(TEAL));
                ui.label(RichText::new("Avg Hold").strong().color(TEAL));
                ui.label(RichText::new("Last Repress").strong().color(TEAL));
                ui.label(RichText::new("Avg Repress").strong().color(TEAL));
                ui.label(RichText::new("State").strong().color(TEAL));
                ui.end_row();

                for info in &infos {
                    ui.label(&info.name);
                    ui.label(format!("{}", info.press_count));
                    ui.label(if info.last_hold_ms > 0.0 {
                        format!("{:.1}ms", info.last_hold_ms)
                    } else {
                        "-".to_string()
                    });
                    ui.label(if info.avg_hold_ms > 0.0 {
                        format!("{:.1}ms", info.avg_hold_ms)
                    } else {
                        "-".to_string()
                    });
                    ui.label(if info.last_repress_ms > 0.0 {
                        format!("{:.1}ms", info.last_repress_ms)
                    } else {
                        "-".to_string()
                    });
                    ui.label(if info.avg_repress_ms > 0.0 {
                        format!("{:.1}ms", info.avg_repress_ms)
                    } else {
                        "-".to_string()
                    });
                    let (state_text, state_color) = if info.held {
                        ("HELD", GREEN)
                    } else {
                        ("--", Color32::DARK_GRAY)
                    };
                    ui.label(RichText::new(state_text).color(state_color));
                    ui.end_row();
                }
            });
    });
}

fn draw_stat(ui: &mut Ui, label: &str, value: &str) {
    ui.horizontal(|ui| {
        ui.label(RichText::new(format!("{label}:")).color(Color32::GRAY));
        ui.label(RichText::new(value).strong());
    });
}
