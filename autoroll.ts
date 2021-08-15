const V8_VERSIONS = [
  "9.3",
  "9.4",
];

// This function runs a specified subcommand and waits until the command exits
// with code 0.
async function run(cmd: string[], cwd: string = "./v8") {
  console.log("$", ...cmd);
  const proc = Deno.run({ cmd, cwd });
  const status = await proc.status();
  if (!status.success) {
    console.error(`Failed to run ${cmd.join(" ")}`);
    Deno.exit(1);
  }
}

async function tryDelete(path: string) {
  try {
    await Deno.remove(path, { recursive: true });
  } catch (err) {
    if (err instanceof Deno.errors.NotFound) {
      return;
    }
    throw err;
  }
}

console.log("Deleting existing V8 clone if it exists.");
await tryDelete("./v8");

console.log("Cloning V8. This might take a couple of minutes.");
await run([
  "git",
  "clone",
  "https://chromium.googlesource.com/v8/v8.git",
  "v8",
], ".");

console.log("Configuring denoland remote.");
await run([
  "git",
  "remote",
  "add",
  "denoland",
  "https://github.com/denoland/v8.git",
]);

console.log("Fetching denoland remote.");
await run(["git", "fetch", "denoland"]);

for (const version of V8_VERSIONS) {
  const UPSTREAM_LKGR = `${version}-lkgr`;
  const DENOLAND_LKGR = `${version}-lkgr-denoland`;

  // Create a $V8_VERSION-lkgr-denoland branch from the upstream
  // $V8_VERSION-lkgr branch.
  await run([
    "git",
    "checkout",
    "-b",
    DENOLAND_LKGR,
    `origin/${UPSTREAM_LKGR}`,
  ]);

  // Get list of all patches in the ../patches directory.
  const patches = [...Deno.readDirSync("./patches")]
    .map((x) => `../patches/${x.name}`);

  for (const patch of patches) {
    // Apply the patch file.
    console.log(`Applying patch ${patch}`);
    await run(["git", "am", patch]);
  }

  // Force push the branch to the denoland remote.
  console.log("Pushing the branch to the remote. This might take a minute.");
  await run(["git", "push", "--force", "denoland", DENOLAND_LKGR]);
}
