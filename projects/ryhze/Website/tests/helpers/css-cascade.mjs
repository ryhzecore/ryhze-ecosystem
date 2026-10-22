import { readFileSync } from 'node:fs';
import * as cssTree from 'css-tree';

// Resolve the actual authored cascade for the layout properties tested below.
// This intentionally is NOT a rendering engine: viewport bounds are arithmetic,
// and font layout, pseudo-state visuals and native browser behavior remain untested.
const sheets = [
  'tokens',
  'base',
  'ambient',
  'components',
  'dropdown',
  'pages',
  'race',
  'entertainment',
].map((name) =>
  cssTree.parse(readFileSync(new URL(`../../src/styles/${name}.css`, import.meta.url), 'utf8')),
);
const variables = {};
const valueText = (value) =>
  value.type === 'Value'
    ? [...value.children].map((node) => cssTree.generate(node)).join(' ')
    : cssTree.generate(value).trim();
for (const rule of sheets[0].children) {
  if (rule.type === 'Rule' && cssTree.generate(rule.prelude) === ':root') {
    for (const declaration of rule.block.children) {
      if (declaration.type === 'Declaration')
        variables[declaration.property] = valueText(declaration.value);
    }
  }
}
const resolve = (value) =>
  value.replace(/var\((--[\w-]+)\)/g, (_, name) =>
    variables[name] ? resolve(variables[name]).trim() : `var(${name})`,
  );
function specificity(selector) {
  let score = 0;
  cssTree.walk(selector, (node) => {
    if (node.type === 'IdSelector') score += 10000;
    if (['ClassSelector', 'AttributeSelector'].includes(node.type)) score += 100;
    if (node.type === 'PseudoClassSelector' && !['not', 'is', 'where'].includes(node.name))
      score += 100;
    if (node.type === 'TypeSelector' && node.name !== '*') score += 1;
  });
  return score;
}
function expand(property, value) {
  if (['padding', 'margin'].includes(property)) {
    const [a, b = a, c = a, d = b] = value.split(/\s+/);
    return Object.fromEntries(
      ['top', 'right', 'bottom', 'left'].map((side, i) => [`${property}-${side}`, [a, b, c, d][i]]),
    );
  }
  if (/^(padding|margin)-(inline|block)$/.test(property)) {
    const [type, direction] = property.split('-');
    const [a, b = a] = value.split(/\s+/);
    const sides = direction === 'inline' ? ['left', 'right'] : ['top', 'bottom'];
    return { [`${type}-${sides[0]}`]: a, [`${type}-${sides[1]}`]: b };
  }
  if (property === 'inset-inline') {
    const [a, b = a] = value.split(/\s+/);
    return { 'inset-inline-start': a, 'inset-inline-end': b };
  }
  return { [property]: value };
}
export function cascade(element, viewport) {
  const result = {};
  const priority = {};
  let order = 0;
  function walk(children) {
    for (const node of children) {
      if (node.type === 'Atrule') {
        if (node.name !== 'media') continue;
        const query = cssTree.generate(node.prelude);
        const constraints = [...query.matchAll(/\((min|max)-width:(\d+)px\)/g)];
        if (
          !constraints.length ||
          !constraints.every(([, kind, size]) =>
            kind === 'min' ? viewport >= Number(size) : viewport <= Number(size),
          )
        )
          continue;
        walk(node.block.children);
      } else if (node.type === 'Rule' && node.prelude.type === 'SelectorList') {
        for (const selector of node.prelude.children) {
          const text = cssTree.generate(selector);
          if (text.includes('::') || /:(hover|active|focus|focus-visible)\b/.test(text)) continue;
          if (!element.matches(text)) continue;
          const score = specificity(selector);
          for (const declaration of node.block.children) {
            if (declaration.type !== 'Declaration') continue;
            const rank = [declaration.important ? 1 : 0, score, ++order];
            for (const [property, value] of Object.entries(
              expand(declaration.property, resolve(valueText(declaration.value))),
            )) {
              const prior = priority[property];
              if (
                !prior ||
                rank[0] > prior[0] ||
                (rank[0] === prior[0] &&
                  (rank[1] > prior[1] || (rank[1] === prior[1] && rank[2] > prior[2])))
              ) {
                result[property] = value;
                priority[property] = rank;
              }
            }
          }
        }
      }
    }
  }
  for (const sheet of sheets) walk(sheet.children);
  return result;
}
