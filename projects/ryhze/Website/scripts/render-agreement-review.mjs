import { mkdir, writeFile } from 'node:fs/promises';
import { draftDocuments } from '../gate/review/drafts.mjs';
import { sha256 } from '../gate/index.mjs';
import { agreementPage } from '../gate/ui/render.mjs';

const documents = await Promise.all(
  draftDocuments.map(async (document) => ({ ...document, sha256: await sha256(document.text) })),
);
const packet = {
  release: '0.0.8',
  documents,
  action: {
    terms: 'Required Terms acceptance will be available only after final approval.',
    nda: 'Required NDA acceptance will be available only after final approval and completion of the tester schedule.',
  },
  optionalFeedback: null,
};
await mkdir(new URL('../.local/', import.meta.url), { recursive: true });
await writeFile(
  new URL('../.local/agreement-review.html', import.meta.url),
  agreementPage({ packet, draft: true }),
);
process.stdout.write(
  'Created .local/agreement-review.html. Local review only; no active acceptance form.\n',
);
