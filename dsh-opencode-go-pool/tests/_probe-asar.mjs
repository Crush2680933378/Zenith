import fs from 'node:fs'

const bundle = 'C:/APP/CodeApp/DeepSeek Harness/resources/app.asar'
const buffer = fs.readFileSync(bundle)
const headerLength = buffer.readUInt32LE(12)
const header = JSON.parse(buffer.slice(16, 16 + headerLength).toString('utf8'))
const base = 16 + headerLength

/** Walk an asar `files` tree, yielding `{ path, entry }` pairs. */
function* walk(node, prefix = '') {
  for (const [name, entry] of Object.entries(node.files ?? {})) {
    const path = `${prefix}/${name}`
    if (entry.files !== undefined) {
      yield* walk(entry, path)
      continue
    }
    yield { path, entry }
  }
}

/** Read one file out of the archive by its `files`-tree path. */
function readFile(entry) {
  const start = base + Number(entry.offset)
  return buffer.slice(start, start + entry.size).toString('utf8')
}

const wanted = [
  'node_modules/@deepseek-ai/dsh-llm-pi-ai/package.json',
  'node_modules/@deepseek-ai/dsh-llm/package.json',
  'node_modules/@deepseek-ai/dsh-credentials/package.json',
  'node_modules/@deepseek-ai/dsh-settings/package.json',
  'node_modules/@deepseek-ai/cordis/package.json',
  'node_modules/@deepseek-ai/schemastery/package.json',
]

const found = new Map()
for (const { path, entry } of walk(header)) {
  for (const want of wanted) {
    if (path.endsWith(`/${want}`)) found.set(want, entry)
  }
}

for (const want of wanted) {
  const entry = found.get(want)
  if (entry === undefined) {
    console.log(`${want.padEnd(58)} MISSING`)
    continue
  }
  const manifest = JSON.parse(readFile(entry))
  console.log(`${manifest.name.padEnd(32)} ${manifest.version}`)
}
