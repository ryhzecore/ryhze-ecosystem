import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import ts from 'typescript';

/** Transpile production controllers and their local dependencies without bundler mocks. */
export function scriptModule(name, globals = {}) {
  const url = new URL(`../../src/scripts/${name}.ts`, import.meta.url);
  const compiled = ts.transpileModule(readFileSync(url, 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  }).outputText;
  const context = {
    exports: {},
    ...globals,
    require: (path) => {
      if (!/^\.\/[\w-]+$/.test(path)) throw new Error(`Unsupported test module: ${path}`);
      return scriptModule(path.slice(2), globals);
    },
  };
  vm.runInNewContext(compiled, context, { filename: `${name}.js` });
  return context.exports;
}
