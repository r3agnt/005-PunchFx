# Ideas (not committed to)

## Reverse fx (from the slice-mode pitch reverse, code removed in 0.9.0)
The old slice-mode "ping-pong" read a slice forward for one 16th and backwards for the next, with no seam (the read position just turns round). New plan: a punch-in **reverse fx** that plays the recent audio BACKWARDS from the current quantised position (no pitch), with the same filter/gain/send controls as the other fx. Possible variants: reverse once, forward/reverse ping-pong, reverse at half speed. Needs a short recording of what just played (post layer 1) and a 5 ms crossfade in and out.
