"""Fit the disc's transparent HD title layout into the GameCube logo UVs."""
from PIL import Image


def title_image(size, artwork, badge=None):
    """Keep transparent pixels and aspect ratio when fitting title layers.

    The English subtitle's legacy plane holds both HD layout panes: its
    wordmark above the centered HD badge. No font or replacement art is made.
    """
    w, h = size
    if badge is None:
        return artwork.resize((w, h), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    word_w = round(w * 0.86)
    word_h = round(word_w * artwork.height / artwork.width)
    badge_w = round(w * 0.264)
    badge_h = round(badge_w * badge.height / badge.width)
    gap = max(1, round(h * 0.04))
    # Fit the two rows together even if a future disc changes their sizes.
    scale = min(1.0, h / (word_h + gap + badge_h))
    word_w, word_h, badge_w, badge_h, gap = [max(1, round(v * scale))
                                           for v in (word_w, word_h, badge_w, badge_h, gap)]
    y = (h - word_h - gap - badge_h) // 2
    canvas.alpha_composite(artwork.resize((word_w, word_h), Image.Resampling.LANCZOS),
                           ((w - word_w) // 2, y))
    canvas.alpha_composite(badge.resize((badge_w, badge_h), Image.Resampling.LANCZOS),
                           ((w - badge_w) // 2, y + word_h + gap))
    return canvas
