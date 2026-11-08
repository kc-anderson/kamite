Checks for mot_edit's motion edits (punish_fit.cpp rewrite(): Gene's pose pre-rotations, the left forearm twist).
apply_fit.cpp applies them exactly as the runtime does (same matrices and order) to a motion blob, per-frame values from
a file. Build: g++ -std=c++20 -O1 -I../../../../src apply_fit.cpp ../../../../src/kamite/mot_edit.cpp -o apply_fit
Run from a scratch directory (they write src.bin / out.bin / vals.txt there), GODHAND_EXTRACT set:
- chk2.py <body hash> <punish>: per motion and bone, the worst error at any half frame (60 FPS) against the intended
  rotation (pre * own * post). Nov 8: <= ~1.4 deg after the half-frame slopes; a branch flip had made 45 deg (Spanking's
  grab, the twitch K saw).
- reg.py: every fit's edited motions, the half-frame wobble beyond the original's own (the originals wobble up to
  ~30 deg at half frames themselves). Nov 8: <= 0.4 deg.
