import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

test("the board-resident routine remains a compact 48-step constant", async () => {
  const header = await readFile(
    "firmware/include/MaterialFarmMacro.h",
    "utf8",
  );
  const arrayBody = header.match(
    /inline constexpr MacroStep kMaterialFarmMacro\[\] = \{([\s\S]*?)\n\};/,
  )?.[1];

  assert.ok(arrayBody, "could not find the embedded routine");
  const steps = [...arrayBody.matchAll(/\{(\d+), (k\w+)\}/g)].map(
    ([, duration, report]) => ({
      duration: Number(duration),
      report,
    }),
  );

  assert.equal(steps.length, 48);
  assert.equal(
    steps.reduce((total, step) => total + step.duration, 0),
    62410,
  );
  assert.ok(steps.every((step) => step.duration > 0));
  assert.equal(steps[0].report, "kReportX");
  assert.equal(steps.at(-1).report, "kReportA");
});
