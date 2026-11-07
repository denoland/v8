const V8_VERSIONS = [
  "14.9",
  "15.0",
  "15.2",
  "15.8",
];

// Patches that only apply to some V8 versions, as an inclusive range.
// - Upstream removed Atomics.Mutex and Atomics.Condition in 15.8, so the
//   padding patch has nothing left to fix there.
// - NegateF16 only exists in the riscv simulator since 15.8.
const PATCH_VERSIONS: Record<string, { min?: string; max?: string }> = {
  "0001-add-padding-to-fix-ptrcmp-builds-on-windows.patch": { max: "15.2" },
  "0004-riscv-Mark-NegateF16-maybe_unused.patch": { min: "15.8" },
};

function compareVersions(a: string, b: string) {
  const [a1, a2] = a.split(".").map(Number);
  const [b1, b2] = b.split(".").map(Number);
  return a1 - b1 || a2 - b2;
}

function patchApplies(name: string, version: string) {
  const range = PATCH_VERSIONS[name];
  if (!range) return true;
  if (range.min && compareVersions(version, range.min) < 0) return false;
  if (range.max && compareVersions(version, range.max) > 0) return false;
  return true;
}

const checkVersions = !!Deno.env.get("CHECK_V8_VERSIONS");

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
async function run(cmd: string, args: string[], cwd: string = "./v8") {
  console.log("$", cmd, ...args);
  const command = new Deno.Command(cmd, { args, cwd });
  const output = await command.output();
  if (!output.success) {
    console.error(`Failed to run ${cmd} ${args.join(" ")}`);
    Deno.exit(1);
  }
}

// This function runs a specified subcommand and waits until the command exits
// with code 0.
async function runAndCollect(
  cmd: string,
  args: string[],
  cwd: string = "./v8",
): Promise<Uint8Array> {
  console.log("$", cmd, ...args);
  const command = new Deno.Command(cmd, { args, cwd, stdout: "piped" });
  const output = await command.output();
  if (!output.success) {
    console.error(`Failed to run ${cmd} ${args.join(" ")}`);
    Deno.exit(1);
  }
  return output.stdout;
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
  await run("git", [
    "clone",
    "https://chromium.googlesource.com/v8/v8.git",
    "v8",
  ], ".");

  console.log("Configuring denoland remote.");
  await run("git", [
    "remote",
    "add",
    "denoland",
    "https://github.com/denoland/v8.git",
  ]);
}

console.log("Fetching denoland remote.");
await run("git", ["fetch", "denoland"]);

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
  await run("git", [
    "checkout",
    "-b",
    DENOLAND_LKGR,
    `origin/${UPSTREAM_LKGR}`,
  ]);

  const versionDotH = await Deno.readTextFile("./v8/include/v8-version.h");
  const upstreamVersion = extractVersion(versionDotH);

  if (checkVersions) {
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
  }
  // Get list of all patches in the ../patches directory.
  const patches = [...Deno.readDirSync("./patches")]
    .filter((x) => patchApplies(x.name, version))
    .map((x) => `../patches/${x.name}`);

  patches.sort();

  for (const patch of patches) {
    // Apply the patch file.
    console.log(`Applying patch ${patch}`);
    await run("git", ["am", "-3", patch]);
  }

  // Force push the branch to the denoland remote.
  console.log("Pushing the branch to the remote. This might take a minute.");
  await run("git", ["push", "--force", "denoland", DENOLAND_LKGR]);

  // Get the current commit.
  const commit = await runAndCollect("git", ["rev-parse", DENOLAND_LKGR]);
  const currentCommit = new TextDecoder().decode(commit).trim();

  // Create a tag for the new version.
  const TAG = `${upstreamVersion}-denoland-${currentCommit.slice(0, 20)}`;
  console.log(`Creating tag ${TAG}`);
  await run("git", ["tag", TAG]);

  // Push the tag to the denoland remote.
  console.log("Pushing the tag to the remote.");
  await run("git", ["push", "denoland", TAG]);
}
