import { randomBytes } from 'node:crypto';
import { defineConfig } from 'vite';

export default defineConfig(({ command }) => {
  // Vite injects CSS during development. Give those style elements a server-
  // lifetime nonce; production CSS stays external and the build deterministic.
  const nonce = command === 'serve' ? randomBytes(16).toString('base64') : undefined;
  return {
    base: './',
    html: { cspNonce: nonce },
    plugins: [{
      name: 'local-development-style-nonce',
      transformIndexHtml(html) {
        return nonce ? html.replace("style-src 'self'", `style-src 'self' 'nonce-${nonce}'`) : html;
      },
    }],
  };
});
