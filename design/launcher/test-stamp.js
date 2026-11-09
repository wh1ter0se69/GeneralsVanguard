// GeneralsX @test Claude 21/09/2026 The THIS INSTALL card's two version cells.
//
// IT READS THE SHIPPED FILE AND RUNS THE FUNCTIONS OUT OF IT. A copy of the logic here would pass
// forever after somebody edited MainWeb.html, which is the exact failure this file exists to catch -
// these screens have had no automated check at all until now, and the Launcher row was dead on every
// platform for five days in September because a ReferenceError was swallowed by a `.catch`.
//
//   node design/launcher/test-stamp.js
//
// No dependencies and no browser: a ~6-line DOM stub covers everything these two functions touch.
'use strict';
const fs = require('fs');
const path = require('path');

// Relative to THIS file, so the suite runs from any working directory.
const MAINWEB = path.join(__dirname, 'screens', 'MainWeb.html');
const src = fs.readFileSync(MAINWEB, 'utf8');

/* Lift one named function out of the page by matching its braces. */
function grab(name) {
	const i = src.indexOf('function ' + name + '(');
	if (i < 0) {
		throw new Error('could not find ' + name + ' in ' + MAINWEB);
	}
	let depth = 0;
	let started = false;
	for (let j = i; j < src.length; j++) {
		if (src[j] === '{') { depth++; started = true; } else if (src[j] === '}') {
			depth--;
			if (started && depth === 0) { return src.slice(i, j + 1); }
		}
	}
	throw new Error('unbalanced braces reading ' + name);
}

function El() { this.children = []; this.style = { cssText: '' }; this.textContent = ''; }
El.prototype.appendChild = function (c) { this.children.push(c); return c; };
El.prototype.removeChild = function (c) { this.children.splice(this.children.indexOf(c), 1); };
Object.defineProperty(El.prototype, 'firstChild', { get() { return this.children[0] || null; } });
global.document = { createElement: () => new El() };

// versionFromChangelog closes over this in the page; declare it here with the same value, and pin
// that they agree so the cap cannot drift out from under the test.
const GX_CHANGELOG_CAP = 200;

// Bound as EXPRESSIONS, not declarations: under 'use strict' an eval'd function declaration stays
// inside eval's own scope and never reaches this one, which fails as `X is not defined` and reads
// like the page is missing the function it just found.
const setStamp = eval('(' + grab('setStamp') + ')');
const versionFromChangelog = eval('(' + grab('versionFromChangelog') + ')');

let pass = 0;
let fail = 0;
function check(name, ok) {
	if (ok) { pass++; console.log('PASS  ' + name); } else { fail++; console.log('FAIL  ' + name); }
}

check('C0 the cap in this test is the cap in the page',
	new RegExp('GX_CHANGELOG_CAP\\s*=\\s*' + GX_CHANGELOG_CAP + '\\b').test(src));

const entries = (n, installedIdx) =>
	Array.from({ length: n }, (_, i) => ({ installed: i === installedIdx, title: 'x' }));

check('V1 the newest of 41, exact -> 0.9.41',
	versionFromChangelog({ installed_exact: true, entries: entries(41, 0) }) === '0.9.41');
check('V2 the 4th newest of 41 -> 0.9.38',
	versionFromChangelog({ installed_exact: true, entries: entries(41, 3) }) === '0.9.38');
check('V3 the OLDEST of 41 is 0.9.1, not 0.9.0',
	versionFromChangelog({ installed_exact: true, entries: entries(41, 40) }) === '0.9.1');
check('V4 installed_exact false -> no number; the entry only SHARES this commit',
	versionFromChangelog({ installed_exact: false, entries: entries(41, 0) }) === '');
check('V5 a list AT the parser cap may be cut short -> no number',
	versionFromChangelog({ installed_exact: true, entries: entries(GX_CHANGELOG_CAP, 0) }) === '');
check('V6 one under the cap is still trustworthy',
	versionFromChangelog({ installed_exact: true, entries: entries(GX_CHANGELOG_CAP - 1, 0) })
		=== '0.9.' + (GX_CHANGELOG_CAP - 1));
check('V7 no entries -> no number',
	versionFromChangelog({ installed_exact: true, entries: [] }) === '');
check('V8 nothing marked installed -> no number',
	versionFromChangelog({ installed_exact: true, entries: entries(41, -1) }) === '');
// The 0.9.53 gap: 54 entries, 0.9.54 at position 53 and 0.9.55 at 54 (counting from the oldest).
const gap = (n, idx, title) => entries(n, idx).map((e, i) => i === idx ? { installed: true, title } : e);
check('V10 titled "GeneralsX 0.9.55" at index 0 of 54 -> 0.9.55, not 0.9.54',
	versionFromChangelog({ installed_exact: true, entries: gap(54, 0, 'GeneralsX 0.9.55') }) === '0.9.55');
check('V11 titled "GeneralsX 0.9.54" at index 1 of 54 -> 0.9.54, not 0.9.53',
	versionFromChangelog({ installed_exact: true, entries: gap(54, 1, 'GeneralsX 0.9.54') }) === '0.9.54');
check('V12 an untitled (summary) entry still falls back to position',
	versionFromChangelog({ installed_exact: true, entries: gap(41, 3, 'Twelve-player fixes') }) === '0.9.38');
check('V13 a sha title is never read as a number',
	versionFromChangelog({ installed_exact: true, entries: gap(41, 0, 'GeneralsX d1f7a859a') }) === '0.9.41');
check('V14 a title that merely starts like a version is not trusted',
	versionFromChangelog({ installed_exact: true, entries: gap(41, 0, 'GeneralsX 0.9.7 fixes') }) === '0.9.41');
// GeneralsX @test Claude 05/10/2026 (Codex codex-batch-1005 p1-launcher) after the gap, an untitled entry counts up from
// the nearest older titled one. 56 entries: index 1 is "GeneralsX 0.9.56", the installed index 0 is a written summary.
const after = (n, titles) => entries(n, 0).map((e, i) => (i in titles ? Object.assign({}, e, { title: titles[i] }) : e));
check('V15 a summary-titled newest entry above "GeneralsX 0.9.56" -> 0.9.57, not its position 0.9.56',
	versionFromChangelog({ installed_exact: true, entries: after(56, { 0: 'Fire timing like Generals Online', 1: 'GeneralsX 0.9.56' }) }) === '0.9.57');
check('V16 two summaries above "GeneralsX 0.9.55" -> the newest is 0.9.57',
	versionFromChangelog({ installed_exact: true, entries: after(56, { 0: 'b', 1: 'a', 2: 'GeneralsX 0.9.55' }) }) === '0.9.57');
check('V17 no titled entry at or below it -> still its position',
	versionFromChangelog({ installed_exact: true, entries: after(41, { 0: 'Twelve-player fixes' }) }) === '0.9.41');
check('V9 a missing or empty changelog never throws',
	versionFromChangelog(null) === '' && versionFromChangelog({}) === '');

const e1 = new El();
setStamp(e1, '0.9.41', 'f24edb9bb');
check('S1 a version over a sha is two lines, in that order',
	e1.children.length === 2
	&& e1.children[0].textContent === '0.9.41'
	&& e1.children[1].textContent === 'f24edb9bb');
check('S2 and the sha line is the smaller, dimmer one',
	/fs-110/.test(e1.children[1].style.cssText) && /ink-dim-3/.test(e1.children[1].style.cssText));

const e2 = new El();
setStamp(e2, 'f24edb9bb', '');
check('S3 a sha with no version is ONE line, exactly as the row rendered before',
	e2.children.length === 1 && e2.children[0].textContent === 'f24edb9bb');

const e3 = new El();
setStamp(e3, 'f24edb9bb', 'f24edb9bb');
check('S4 the same value is never printed twice', e3.children.length === 1);

setStamp(e1, '0.9.41', 'f24edb9bb');
setStamp(e1, '0.9.41', 'f24edb9bb');
check('S5 repainting replaces the cell rather than growing it', e1.children.length === 2);

// The regression that made this file worth writing: a painter that used textContent would silently
// delete the sha line. Nothing may write these cells except setStamp.
check('S6 (fail-arm) no painter writes buildEl or launcherEl through textContent',
	!/\b(buildEl|launcherEl)\.textContent\s*=/.test(src));

console.log('\n' + (pass + fail) + ' checks, ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
