// Scenario driver for tools/clip/scenarios/record.sh (macOS): posts keyboard
// and mouse events, one command per line of a script (stdin or a file). It
// needs the Accessibility permission (for the terminal it runs from), and it
// types and clicks into the frontmost app: every command that does checks
// first that the app named by `front` is still in front, and stops
// otherwise (exit 3), so nothing goes into another app's window.
//
//   swiftc -O drive.swift -o drive && ./drive SCRIPT
//
// Lines:
//   front BUNDLE_ID       stop unless that app is frontmost (checked again before every key and click)
//   activate BUNDLE_ID    bring the app to the front
//   sleep MS
//   type CPS TEXT         type TEXT at CPS characters a second ("\n" = Return)
//   key COMBO             e.g. cmd+tab, ctrl+cmd+f, return, space, escape
//   scroll DY N MS        N wheel events of DY pixels, MS apart (negative = down)
//   move X Y
//   click X Y
//   drag MS X1 Y1 X2 Y2 [X3 Y3 ...]   mouse down at the first point, along the path, up at the last
//   ime ID                select an input source (com.apple.keylayout.ABC, ...)
//   ime-get               print the input source selected now
//   # comment
// Lines are trimmed: a space at the end of `type` is lost (use `key space`).
import AppKit
import Carbon

var expected: String? = nil
let src = CGEventSource(stateID: .hidSystemState)

func fail(_ s: String) -> Never { FileHandle.standardError.write((s + "\n").data(using: .utf8)!); exit(3) }
func frontName() -> String { NSWorkspace.shared.frontmostApplication?.bundleIdentifier ?? "" }
func checkFront() { if let e = expected, frontName() != e { fail("frontmost app is \(frontName()), not \(e): stopped") } }
func ms(_ v: Double) { usleep(useconds_t(max(0, v) * 1000)) }

let keyCodes: [String: CGKeyCode] = ["return": 36, "tab": 48, "space": 49, "delete": 51, "escape": 53,
  "left": 123, "right": 124, "down": 125, "up": 126, "f": 3, "w": 13, "n": 45, "q": 12, "a": 0, "home": 115, "end": 119]

func key(_ combo: String) {
  checkFront()
  var flags: CGEventFlags = []
  var code: CGKeyCode? = nil
  for p in combo.split(separator: "+").map(String.init) {
    switch p {
    case "cmd": flags.insert(.maskCommand)
    case "ctrl": flags.insert(.maskControl)
    case "alt": flags.insert(.maskAlternate)
    case "shift": flags.insert(.maskShift)
    default: code = keyCodes[p]
    }
  }
  guard let c = code else { fail("unknown key \(combo)") }
  let d = CGEvent(keyboardEventSource: src, virtualKey: c, keyDown: true)!; d.flags = flags; d.post(tap: .cghidEventTap)
  ms(30)
  let u = CGEvent(keyboardEventSource: src, virtualKey: c, keyDown: false)!; u.flags = flags; u.post(tap: .cghidEventTap)
  ms(30)
}

// ASCII letters, digits and punctuation go as real key codes (an input
// method only sees those); anything else as a Unicode string.
let asciiKeys: [Character: (CGKeyCode, Bool)] = {
  var m: [Character: (CGKeyCode, Bool)] = [:]
  let lower = "asdfhgzxcv bqweryt123465=97-80]ou[ip lj'k;\\,/nm.  `"
  let codes: [CGKeyCode] = [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50]
  for (i, ch) in lower.enumerated() where ch != " " { m[ch] = (codes[i], false) }
  let shifted: [Character: Character] = ["!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "-", "+": "=", "{": "[", "}": "]", "|": "\\", ":": ";", "\"": "'", "<": ",", ">": ".", "?": "/", "~": "`"]
  for (s, b) in shifted { if let k = m[b] { m[s] = (k.0, true) } }
  for ch in "abcdefghijklmnopqrstuvwxyz" { if let k = m[ch] { m[Character(ch.uppercased())] = (k.0, true) } }
  m[" "] = (49, false)
  return m
}()

func typeChar(_ ch: Character) {
  checkFront()
  if ch == "\n" { key("return"); return }
  if let (code, shift) = asciiKeys[ch] {
    let d = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: true)!
    let u = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: false)!
    // Always set: an event from the HID state source otherwise keeps the
    // modifiers of the events before it (one capital left Shift down).
    d.flags = shift ? .maskShift : []; u.flags = shift ? .maskShift : []
    d.post(tap: .cghidEventTap); ms(15); u.post(tap: .cghidEventTap)
    return
  }
  let units = Array(String(ch).utf16)
  let d = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: true)!
  d.keyboardSetUnicodeString(stringLength: units.count, unicodeString: units)
  d.flags = []
  let u = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: false)!
  u.flags = []
  u.keyboardSetUnicodeString(stringLength: units.count, unicodeString: units)
  d.post(tap: .cghidEventTap); ms(15); u.post(tap: .cghidEventTap)
}

func mouse(_ type: CGEventType, _ x: Double, _ y: Double) {
  CGEvent(mouseEventSource: src, mouseType: type, mouseCursorPosition: CGPoint(x: x, y: y), mouseButton: .left)!.post(tap: .cghidEventTap)
}

func selectInputSource(_ id: String) {
  let props = [kTISPropertyInputSourceID as String: id] as CFDictionary
  guard let list = TISCreateInputSourceList(props, false)?.takeRetainedValue() as? [TISInputSource], let s = list.first else { fail("no input source \(id)") }
  TISSelectInputSource(s)
}

let lines: [String]
if CommandLine.arguments.count > 1 { lines = (try? String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8))?.components(separatedBy: "\n") ?? [] }
else { var a: [String] = []; while let l = readLine() { a.append(l) }; lines = a }

for raw in lines {
  let line = raw.trimmingCharacters(in: .whitespaces)
  if line.isEmpty || line.hasPrefix("#") { continue }
  let parts = line.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: true).map(String.init)
  let rest = line.split(separator: " ", maxSplits: 1).count > 1 ? String(line.split(separator: " ", maxSplits: 1)[1]) : ""
  switch parts[0] {
  case "front": expected = rest; checkFront()
  case "activate":
    guard let app = NSWorkspace.shared.runningApplications.first(where: { $0.bundleIdentifier == rest }) else { fail("\(rest) is not running") }
    app.activate(options: [])
    ms(500)
  case "sleep": ms(Double(rest) ?? 0)
  case "type":
    let cps = Double(parts[1]) ?? 8
    let text = (parts.count > 2 ? parts[2] : "").replacingOccurrences(of: "\\n", with: "\n")
    for ch in text { typeChar(ch); ms(1000 / cps - 15) }
  case "key": key(rest)
  case "scroll":
    let dy = Int32(parts[1]) ?? -40
    let rp = rest.split(separator: " ").map { Double($0) ?? 0 }
    let n = Int(rp.count > 1 ? rp[1] : 1), gap = rp.count > 2 ? rp[2] : 16
    for _ in 0..<n {
      checkFront()
      CGEvent(scrollWheelEvent2Source: src, units: .pixel, wheelCount: 1, wheel1: dy, wheel2: 0, wheel3: 0)!.post(tap: .cghidEventTap)
      ms(gap)
    }
  case "move":
    let p = rest.split(separator: " ").map { Double($0) ?? 0 }; mouse(.mouseMoved, p[0], p[1])
  case "click":
    checkFront()
    let p = rest.split(separator: " ").map { Double($0) ?? 0 }
    mouse(.mouseMoved, p[0], p[1]); ms(50); mouse(.leftMouseDown, p[0], p[1]); ms(60); mouse(.leftMouseUp, p[0], p[1])
  case "drag":
    checkFront()
    let v = rest.split(separator: " ").map { Double($0) ?? 0 }
    let total = v[0]; var pts: [(Double, Double)] = []
    var i = 1; while i + 1 < v.count { pts.append((v[i], v[i + 1])); i += 2 }
    mouse(.mouseMoved, pts[0].0, pts[0].1); ms(80); mouse(.leftMouseDown, pts[0].0, pts[0].1); ms(120)
    let steps = max(1, Int(total / 16)), segs = pts.count - 1
    for s in 1...steps {
      let f = Double(s) / Double(steps) * Double(segs); let k = min(segs - 1, Int(f)); let r = f - Double(k)
      let x = pts[k].0 + (pts[k + 1].0 - pts[k].0) * r, y = pts[k].1 + (pts[k + 1].1 - pts[k].1) * r
      mouse(.leftMouseDragged, x, y); ms(16)
    }
    ms(80); mouse(.leftMouseUp, pts.last!.0, pts.last!.1)
  case "ime": selectInputSource(rest)
  case "ime-get":
    let s = TISCopyCurrentKeyboardInputSource().takeRetainedValue()
    print(Unmanaged<CFString>.fromOpaque(TISGetInputSourceProperty(s, kTISPropertyInputSourceID)).takeUnretainedValue())
  default: fail("unknown command: \(line)")
  }
}
