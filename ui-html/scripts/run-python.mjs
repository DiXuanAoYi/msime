import { spawnSync } from 'node:child_process';

// Windows' python3 app-execution alias may open the Store instead of Python.
// The explicit developer bootstrap shares its verified interpreter with pnpm.
const command = process.env.MSIME_PYTHON || (process.platform === 'win32' ? 'python' : 'python3');
const result = spawnSync(command, process.argv.slice(2), { stdio: 'inherit', shell: false });
if (result.error) {
  console.error(`Python could not start (${command}): ${result.error.message}`);
}
process.exit(result.status ?? 1);
