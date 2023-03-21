const V8_VERSIONS = [
  "11.2",
];

// Extract the V8 version from the include/v8-version.h file.
function extractVersion(versionDotH: string) {
  const MAJOR_PREFIX = "#define V8_MAJOR_VERSION ";
  const MINOR_PREFIX = "#define V8_MINOR_VERSION ";
  const BUILD_PREFIX = "#define V8_BUILD_NUMBER ";
  const PATCH_PREFIX = "#define V8_PATCH_LEVEL ";

  const lines = versionDotH.split("\n");
  const major = parseInt(lines.find((s) => s.startsWith(MAJOR_PREFIX))!
    .substring(MAJOR_PREFIX.length));
  const minor = parseInt(lines.find((s) => s.startsWith(MINOR_PREFIX))!
    .substring(MINOR_PREFIX.length));
  const build = parseInt(lines.find((s) => s.startsWith(BUILD_PREFIX))!
    .substring(BUILD_PREFIX.length));
  const patch = parseInt(lines.find((s) => s.startsWith(PATCH_PREFIX))!
    .substring(PATCH_PREFIX.length));

  return `${major}.${minor}.${build}.${patch}`;
}

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

// This function runs a specified subcommand and waits until the command exits
// with code 0.
async function runAndCollect(
  cmd: string[],
  cwd: string = "./v8",
): Promise<Uint8Array> {
  console.log("$", ...cmd);
  const proc = Deno.run({ cmd, cwd, stdout: "piped" });
  const status = await proc.status();
  if (!status.success) {
    console.error(`Failed to run ${cmd.join(" ")}`);
    Deno.exit(1);
  }
  return await proc.output();
}

async function pathExists(path: string): Promise<boolean> {
  try {
    await Deno.stat(path);
    return true;
  } catch {
    return false;
  }
}

if (!await pathExists("./v8/.git")) {
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
}

console.log("Fetching denoland remote.");
await run(["git", "fetch", "denoland"]);

for (const version of V8_VERSIONS) {
  const UPSTREAM_LKGR = `${version}-lkgr`;
  const DENOLAND_LKGR = `${version}-lkgr-denoland`;

  let currentVersion = null;
  const resp = await fetch(
    `https://api.github.com/repos/denoland/v8/contents/include/v8-version.h?ref=${DENOLAND_LKGR}`,
    {
      headers: {
        "User-Agent": "denoland/v8 auto updater",
        Accept: "application/vnd.github.v3.raw",
      },
    },
  );
  if (resp.status === 200) {
    const versionDotH = await resp.text();
    currentVersion = extractVersion(versionDotH);
  }

  // Create a $V8_VERSION-lkgr-denoland branch from the upstream
  // $V8_VERSION-lkgr branch.
  await run([
    "git",
    "checkout",
    "-b",
    DENOLAND_LKGR,
    `origin/${UPSTREAM_LKGR}`,
  ]);

  const versionDotH = await Deno.readTextFile("./v8/include/v8-version.h");
  const upstreamVersion = extractVersion(versionDotH);

  // If the upstream version does not match the current version, then we need to
  // roll.
  if (upstreamVersion === currentVersion) {
    console.log(
      `Upstream version ${upstreamVersion} matches current version ${currentVersion}. No need to roll ${UPSTREAM_LKGR}.`,
    );
    continue;
  }

  console.log(
    `Upstream version ${upstreamVersion} does not match current version ${currentVersion}. Rolling ${UPSTREAM_LKGR}...`,
  );

  // Get list of all patches in the ../patches directory.
  const patches = [...Deno.readDirSync("./patches")]
    .map((x) => `../patches/${x.name}`);

  for (const patch of patches) {
    // Apply the patch file.
    console.log(`Applying patch ${patch}`);
    await run(["git", "am", "-3", patch]);
  }

  // Force push the branch to the denoland remote.
  console.log("Pushing the branch to the remote. This might take a minute.");
  await run(["git", "push", "--force", "denoland", DENOLAND_LKGR]);

  // Get the current commit.
  const commit = await runAndCollect(["git", "rev-parse", DENOLAND_LKGR]);
  const currentCommit = new TextDecoder().decode(commit).trim();

  // Create a tag for the new version.
  const TAG = `${upstreamVersion}-denoland-${currentCommit.slice(0, 20)}`;
  console.log(`Creating tag ${TAG}`);
  await run(["git", "tag", TAG]);

  // Push the tag to the denoland remote.
  console.log("Pushing the tag to the remote.");
  await run(["git", "push", "denoland", TAG]);
}
