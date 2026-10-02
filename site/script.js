// ---------------------------------------------------------------------------
// LEXIS landing page
// ---------------------------------------------------------------------------
// DOWNLOAD URL: temporary. Points at the GitHub releases page until the
// signed DMG is uploaded to the Cloudflare R2 bucket, at which point swap
// this to the R2 public URL, e.g.
//   https://downloads.<domain>/LEXIS-signed.dmg
// ---------------------------------------------------------------------------
const DOWNLOAD_URL = 'https://pub-7a84f9c1213f46d2bf2d696712dc9f29.r2.dev/LEXIS-signed.dmg';

document.querySelectorAll('.download-btn').forEach((a) => {
  a.setAttribute('href', DOWNLOAD_URL);
});

// Sticky nav state
const nav = document.getElementById('nav');
const onScroll = () => nav.classList.toggle('scrolled', window.scrollY > 24);
window.addEventListener('scroll', onScroll, { passive: true });
onScroll();

// Scroll reveals
const revealObs = new IntersectionObserver(
  (entries) => entries.forEach((e) => {
    if (e.isIntersecting) { e.target.classList.add('visible'); revealObs.unobserve(e.target); }
  }),
  { threshold: 0.12 }
);
document.querySelectorAll('.reveal').forEach((el, i) => {
  el.style.transitionDelay = Math.min(i % 4, 3) * 70 + 'ms';
  revealObs.observe(el);
});

// Animated stat counters
const countObs = new IntersectionObserver(
  (entries) => entries.forEach((e) => {
    if (!e.isIntersecting) return;
    const el = e.target;
    countObs.unobserve(el);
    const target = parseFloat(el.dataset.target);
    const dec = parseInt(el.dataset.dec || '0', 10);
    const dur = 1400;
    const t0 = performance.now();
    const tick = (t) => {
      const p = Math.min((t - t0) / dur, 1);
      const eased = 1 - Math.pow(1 - p, 3);
      el.textContent = (target * eased).toFixed(dec);
      if (p < 1) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }),
  { threshold: 0.5 }
);
document.querySelectorAll('.count').forEach((el) => countObs.observe(el));

// FAQ accordion
document.querySelectorAll('.faq-item').forEach((item) => {
  const q = item.querySelector('.faq-q');
  const a = item.querySelector('.faq-a');
  q.addEventListener('click', () => {
    const open = item.classList.contains('open');
    document.querySelectorAll('.faq-item.open').forEach((o) => {
      o.classList.remove('open');
      o.querySelector('.faq-a').style.maxHeight = null;
    });
    if (!open) {
      item.classList.add('open');
      a.style.maxHeight = a.scrollHeight + 'px';
    }
  });
});
