(() => {
  'use strict';
  const markdown = window.markdownit({ html: false, linkify: true, typographer: false });
  const escape = markdown.utils.escapeHtml;
  const aliases = {
    'c++': 'cpp', cc: 'cpp', cxx: 'cpp', hpp: 'cpp', 'c#': 'csharp', cs: 'csharp',
    py: 'python', js: 'javascript', ts: 'typescript', sh: 'shellscript', bash: 'shellscript',
    shell: 'shellscript', zsh: 'shellscript', ps: 'powershell', ps1: 'powershell', pwsh: 'powershell',
    yml: 'yaml', md: 'markdown', rs: 'rust', golang: 'go', docker: 'dockerfile',
    h: 'c', htm: 'html', txt: 'text', plaintext: 'text', console: 'text'
  };
  const languages = new Set(['c', 'cpp', 'python', 'json', 'jsonc', 'javascript', 'typescript',
    'jsx', 'tsx', 'html', 'css', 'shellscript', 'powershell', 'sql', 'yaml', 'markdown',
    'xml', 'java', 'csharp', 'rust', 'go', 'cmake', 'dockerfile', 'toml', 'diff']);
  const asset = '/vendor/chat-markdown/';
  let highlighterPromise;
  const languagePromises = new Map();

  function getHighlighter() {
    if (!highlighterPromise) highlighterPromise = Promise.all([
      import(asset + 'core.mjs'), import(asset + 'engine.mjs'), import(asset + 'dark-plus.mjs')
    ]).then(([core, engine, theme]) => core.createHighlighterCore({
      themes: [theme.default], langs: [], engine: engine.createOnigurumaEngine(import(asset + 'wasm.mjs'))
    }));
    return highlighterPromise;
  }

  async function highlight(code, text, language) {
    if (!languages.has(language)) return;
    try {
      const highlighter = await getHighlighter();
      if (!languagePromises.has(language)) languagePromises.set(language,
        import(asset + 'lang-' + language + '.mjs').then((grammar) => highlighter.loadLanguage(grammar.default)));
      await languagePromises.get(language);
      if (!code.isConnected) return;
      const result = highlighter.codeToTokens(text, { lang: language, theme: 'dark-plus' });
      const fragment = document.createDocumentFragment();
      result.tokens.forEach((line, index) => {
        if (index) fragment.append(document.createTextNode('\n'));
        line.forEach((token) => {
          const span = document.createElement('span');
          span.textContent = token.content;
          if (token.color) span.style.color = token.color;
          if (token.fontStyle & 1) span.style.fontStyle = 'italic';
          if (token.fontStyle & 2) span.style.fontWeight = 'bold';
          if (token.fontStyle & 4) span.style.textDecoration = 'underline';
          fragment.append(span);
        });
      });
      // Keep the exact code text if a grammar/tokenizer ever normalizes it.
      if (fragment.textContent !== text) return;
      code.replaceChildren(fragment);
    } catch {
      // Plain text and Copy remain usable if highlighting is unavailable.
    }
  }

  const linkOpen = markdown.renderer.rules.link_open;
  markdown.renderer.rules.link_open = (tokens, index, options, env, self) => {
    tokens[index].attrSet('target', '_blank');
    tokens[index].attrSet('rel', 'noopener noreferrer');
    return linkOpen ? linkOpen(tokens, index, options, env, self) : self.renderToken(tokens, index, options);
  };
  // Render image references as links, without fetching remote images from model output.
  markdown.renderer.rules.image = (tokens, index) => {
    const token = tokens[index];
    return '<a target="_blank" rel="noopener noreferrer" href="' + escape(token.attrGet('src') || '') + '">' +
      escape(token.content || 'Изображение') + '</a>';
  };
  const codeBlock = (tokens, index, options, env) => {
    const token = tokens[index];
    const label = (token.info || '').trim().split(/\s+/)[0] || 'text';
    const key = env.blocks.length;
    env.blocks.push({ text: token.content, language: aliases[label.toLowerCase()] || label.toLowerCase() });
    return '<section class="md-code-block" data-code-block="' + key + '">' +
      '<div class="md-code-toolbar"><span class="md-code-language">' + escape(label) + '</span>' +
      '<button class="md-code-copy" type="button" aria-live="polite">Copy</button></div>' +
      '<pre tabindex="0"><code>' + escape(token.content) + '</code></pre></section>\n';
  };
  markdown.renderer.rules.fence = codeBlock;
  markdown.renderer.rules.code_block = codeBlock;

  async function copyText(text) {
    if (navigator.clipboard?.writeText) {
      try { await navigator.clipboard.writeText(text); return; } catch { /* Try the local fallback. */ }
    }
    const textarea = document.createElement('textarea');
    textarea.value = text;
    textarea.className = 'md-copy-buffer';
    textarea.setAttribute('aria-hidden', 'true');
    document.body.append(textarea);
    textarea.select();
    try { if (!document.execCommand('copy')) throw new Error('Clipboard unavailable'); }
    finally { textarea.remove(); }
  }

  window.renderAssistantMarkdown = (element, source) => {
    const env = { blocks: [] };
    try {
      // Raw HTML is disabled; markdown-it escapes text and validates URL protocols.
      element.innerHTML = markdown.render(source, env);
      element.classList.add('markdown-body');
    } catch { element.textContent = source; return; }
    element.querySelectorAll('[data-code-block]').forEach((block) => {
      const entry = env.blocks[Number(block.dataset.codeBlock)];
      const code = block.querySelector('code');
      const button = block.querySelector('button');
      button.setAttribute('aria-label', 'Копировать код');
      button.addEventListener('click', async () => {
        button.disabled = true;
        try { await copyText(entry.text); button.textContent = 'Copied'; }
        catch { button.textContent = 'Copy failed'; }
        finally {
          button.disabled = false;
          button.focus({ preventScroll: true });
          setTimeout(() => { button.textContent = 'Copy'; }, 1800);
        }
      });
      void highlight(code, entry.text, entry.language);
    });
    element.querySelectorAll('table').forEach((table) => {
      const wrapper = document.createElement('div');
      wrapper.className = 'md-table-scroll';
      wrapper.tabIndex = 0;
      table.replaceWith(wrapper);
      wrapper.append(table);
    });
  };
})();
