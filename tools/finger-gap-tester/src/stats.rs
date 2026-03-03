const BUCKET_EDGES: &[f64] = &[0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 10.0, 12.0, 15.0, 20.0, 50.0];

pub struct GapStats {
    gaps: Vec<f64>,
}

impl GapStats {
    pub fn new() -> Self {
        Self { gaps: Vec::new() }
    }

    pub fn record(&mut self, gap_ms: f64) {
        self.gaps.push(gap_ms);
    }

    pub fn count(&self) -> usize {
        self.gaps.len()
    }

    pub fn average(&self) -> f64 {
        if self.gaps.is_empty() {
            return 0.0;
        }
        self.gaps.iter().sum::<f64>() / self.gaps.len() as f64
    }

    pub fn median(&self) -> f64 {
        if self.gaps.is_empty() {
            return 0.0;
        }
        let mut sorted = self.gaps.clone();
        sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());
        sorted[sorted.len() / 2]
    }

    pub fn min(&self) -> f64 {
        self.gaps.iter().cloned().fold(f64::INFINITY, f64::min)
    }

    pub fn max(&self) -> f64 {
        self.gaps.iter().cloned().fold(0.0f64, f64::max)
    }

    pub fn last_gap(&self) -> Option<f64> {
        self.gaps.last().copied()
    }

    /// Returns (label, count, percentage) for each histogram bucket.
    pub fn histogram_buckets(&self) -> Vec<(String, usize, f64)> {
        let n = self.gaps.len();
        if n == 0 {
            return Vec::new();
        }

        let num_buckets = BUCKET_EDGES.len(); // last bucket is 20-50+
        let mut counts = vec![0usize; num_buckets];

        for &g in &self.gaps {
            let mut placed = false;
            for i in 0..num_buckets - 1 {
                if g < BUCKET_EDGES[i + 1] {
                    counts[i] = counts[i].saturating_add(1);
                    placed = true;
                    break;
                }
            }
            if !placed {
                counts[num_buckets - 1] = counts[num_buckets - 1].saturating_add(1);
            }
        }

        let mut result = Vec::new();
        for i in 0..num_buckets - 1 {
            let label = format!("{}-{}ms", BUCKET_EDGES[i] as u32, BUCKET_EDGES[i + 1] as u32);
            let pct = counts[i] as f64 / n as f64 * 100.0;
            result.push((label, counts[i], pct));
        }
        // Last bucket: 20ms+
        let label = format!("{}ms+", BUCKET_EDGES[num_buckets - 1] as u32);
        let pct = counts[num_buckets - 1] as f64 / n as f64 * 100.0;
        result.push((label, counts[num_buckets - 1], pct));

        result
    }

    /// Recommended NOBD slider value: max(8, floor(max_gap) + 2)
    pub fn recommended_nobd(&self) -> u32 {
        if self.gaps.is_empty() {
            return 0;
        }
        let mx = self.max();
        8u32.max(mx as u32 + 2)
    }

    pub fn clear(&mut self) {
        self.gaps.clear();
    }
}
