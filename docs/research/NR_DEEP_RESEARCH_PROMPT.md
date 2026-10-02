# Deep research prompt: original non-generative noise reduction

Conduct a deep research review for an original, non-generative noise-reduction algorithm or trained model for **LibRawOps**, a Windows C++ RAW-image engine.

We have immutable bilinear and opt-in Menon demosaicing, scene-linear float32 RGB with signed shadows and highlight headroom, tiled rendering, cached edit graphs, and reduced-resolution previews. Our current research uses calibrated native-resolution ProPhoto/D50 RGB before tone mapping or clipping. We have eleven Nikon Z7 II RAW files, including ISO 3200 and 20000, and fixed background, skin, fabric, and printed-detail crops. These are ordinary photographs, without matched clean/noisy truth or a calibrated sensor-noise model.

The goal is noise reduction that preserves real photographic detail. Consider both classical algorithms and discriminative learned models, including noise-residual estimation. Exclude diffusion, GAN-based enhancement, text-conditioned restoration, and generative detail synthesis. Explain what “non-generative” does and does not guarantee about false detail or erased texture.

Research:

1. Compare credible classical and learned approaches. Explain their assumptions, failure modes, intensity/color treatment, suitability for RAW-derived noise, and computational costs.
2. Compare filtering the Bayer mosaic, camera-linear reconstructed RGB, and calibrated working-space RGB. Recommend a first prototype domain and processing order, including white balance, demosaicing, preview downsampling, tone mapping, and sharpening.
3. Describe how to build an original prototype and, separately, how to train a model. Specify data requirements, noise simulation limits, clean/noisy capture methods, losses, evaluation splits, and leakage risks. Distinguish Gaussian benchmark success from real-camera performance.
4. Define tests for noise removal, texture loss, edge damage, false color, halos, invented detail, signed/headroom behavior, and consistency between previews and exports. Include known-truth synthetic tests and visual inspection.
5. Address integration into a tiled C++ engine: neighborhood/receptive field, tile overlap and seams, border handling, bounded memory, CPU/GPU execution, cancellation, model identity, and reproducibility.
6. Review exact source-code, dependency, dataset, and model-weight licenses for proprietary distribution. Separate copyright permission from patent questions. Identify unresolved questions rather than declaring an approach legally safe.
7. Recommend a small first experiment, a staged implementation plan, and explicit criteria for continuing or rejecting a candidate.

Use primary papers, author repositories, and official documentation. Cite links and exact versions where relevant. Clearly distinguish established evidence from your recommendations. End with a comparison table, a preferred first prototype, and the additional data we would need. Do not assume that our current photographs are sufficient to train or certify a production model.

Our parallel engineering work is establishing a transparent square-mean smoothing control with independent intensity/color strengths and known-truth fixtures. Treat it as an evaluation baseline, not a preselected production method. The engine's existing preview blur runs after downsampling; a future native-before-downsampling NR stage still needs an explicit contract.
