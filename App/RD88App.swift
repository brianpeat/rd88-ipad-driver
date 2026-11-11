import SwiftUI
import CoreMIDI

@main
struct RD88App: App {
    var body: some Scene {
        WindowGroup { ContentView() }
    }
}

/// Talks to the RD-88 over MIDI: lists the MIDI devices the iPad sees, shows incoming
/// messages, and reads/writes the keyboard's system settings with Roland SysEx
/// (addresses from Roland's "RD-88 MIDI Implementation" document, version 1.12).
final class RD88Link: ObservableObject {
    @Published var sources: [String] = []
    @Published var destinations: [String] = []
    @Published var log: [String] = []
    @Published var status = ""

    /// Bytes of the keyboard's System area that we have read or written, by address.
    @Published private(set) var memory: [UInt32: UInt8] = [:]

    private var client = MIDIClientRef()
    private var inPort = MIDIPortRef()
    private var outPort = MIDIPortRef()
    private var connected = Set<MIDIEndpointRef>()
    private var sysex: [UInt8] = []       // touched only on the MIDI thread

    // Addresses in the RD-88's System area.
    enum Address {
        static let systemCommon: UInt32 = 0x0000_0000   // 0x25 bytes
        static let systemControl: UInt32 = 0x0000_0100  // 0x18 bytes
        static let masterTune: UInt32 = 0x0000_0000     // 4 nibbles, 24...2024 = -100.0...+100.0 cents
        static let keyShift: UInt32 = 0x0000_0004       // 40...88 = -24...+24
        static let localSwitch: UInt32 = 0x0000_0023    // 0 off, 1 on
        static let velocity: UInt32 = 0x0000_0102       // 0 = real, 1...127 = fixed
        static let velocityCurve: UInt32 = 0x0000_0103  // 0...4
        static let velocityOffset: UInt32 = 0x0000_0104 // 54...73 = -10...+9
    }

    init() {
        var err = MIDIClientCreateWithBlock("RD88 Monitor" as CFString, &client) { [weak self] _ in
            DispatchQueue.main.async { self?.refresh() }
        }
        if err != noErr { status = "MIDI client error \(err)" }
        err = MIDIInputPortCreateWithProtocol(client, "In" as CFString, ._1_0, &inPort) { [weak self] list, _ in
            self?.receive(list)
        }
        if err != noErr { status = "MIDI input port error \(err)" }
        MIDIOutputPortCreate(client, "Out" as CFString, &outPort)
        refresh()
    }

    // MARK: Devices

    private static func name(_ endpoint: MIDIEndpointRef) -> String {
        var value: Unmanaged<CFString>?
        MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &value)
        return (value?.takeRetainedValue() as String?) ?? "(unnamed)"
    }

    private var rd88Destinations: [MIDIEndpointRef] {
        (0..<MIDIGetNumberOfDestinations()).map { MIDIGetDestination($0) }
            .filter { RD88Link.name($0).contains("RD-88") }
    }

    var keyboardConnected: Bool { destinations.contains { $0.contains("RD-88") } }

    func refresh() {
        var names: [String] = []
        for i in 0..<MIDIGetNumberOfSources() {
            let source = MIDIGetSource(i)
            names.append(RD88Link.name(source))
            if !connected.contains(source) {
                MIDIPortConnectSource(inPort, source, nil)
                connected.insert(source)
            }
        }
        sources = names
        destinations = (0..<MIDIGetNumberOfDestinations()).map { RD88Link.name(MIDIGetDestination($0)) }
    }

    // MARK: Receiving

    private func receive(_ list: UnsafePointer<MIDIEventList>) {
        var lines: [String] = []
        var messages: [[UInt8]] = []
        for packet in list.unsafeSequence() {
            let count = Int(packet.pointee.wordCount)
            let base = UnsafeRawPointer(packet).advanced(by: MemoryLayout<MIDIEventPacket>.offset(of: \.words) ?? 12)
            let words: [UInt32] = (0..<count).map { base.loadUnaligned(fromByteOffset: $0 * 4, as: UInt32.self) }
            var i = 0
            while i < words.count {
                let word = words[i]
                let type = word >> 28
                if type == 3, i + 1 < words.count {          // 7-bit system exclusive, two words
                    let second = words[i + 1]
                    let form = (word >> 20) & 0xF
                    let n = min(Int((word >> 16) & 0xF), 6)
                    let bytes: [UInt8] = [UInt8((word >> 8) & 0x7F), UInt8(word & 0x7F),
                                          UInt8((second >> 24) & 0x7F), UInt8((second >> 16) & 0x7F),
                                          UInt8((second >> 8) & 0x7F), UInt8(second & 0x7F)]
                    if form == 0 || form == 1 { sysex.removeAll() }
                    sysex.append(contentsOf: bytes.prefix(n))
                    if form == 0 || form == 3 {
                        messages.append(sysex)
                        lines.append("SysEx, \(sysex.count) bytes")
                        sysex.removeAll()
                    }
                    i += 2
                    continue
                }
                var text = String(format: "%08X", word)
                if type == 2 {
                    let statusByte = UInt8((word >> 16) & 0xFF)
                    let d1 = UInt8((word >> 8) & 0x7F)
                    let d2 = UInt8(word & 0x7F)
                    let channel = (statusByte & 0x0F) + 1
                    switch statusByte & 0xF0 {
                    case 0x90: text += d2 > 0 ? "  note on  \(d1)  velocity \(d2)  ch \(channel)" : "  note off \(d1)  ch \(channel)"
                    case 0x80: text += "  note off \(d1)  ch \(channel)"
                    case 0xB0: text += "  control \(d1) = \(d2)  ch \(channel)"
                    case 0xC0: text += "  program \(d1)  ch \(channel)"
                    case 0xE0: text += "  pitch bend  ch \(channel)"
                    default: break
                    }
                }
                lines.append(text)
                i += 1
            }
        }
        DispatchQueue.main.async {
            for message in messages { self.handleSysex(message) }
            self.log.insert(contentsOf: lines.reversed(), at: 0)
            if self.log.count > 40 { self.log.removeLast(self.log.count - 40) }
        }
    }

    /// Roland "Data Set 1": 41 dev 00 00 00 64 12 address(4) data... checksum (no F0/F7 here).
    private func handleSysex(_ m: [UInt8]) {
        guard m.count >= 13, m[0] == 0x41, Array(m[2...5]) == [0x00, 0x00, 0x00, 0x64], m[6] == 0x12 else { return }
        let address = UInt32(m[7]) << 24 | UInt32(m[8]) << 16 | UInt32(m[9]) << 8 | UInt32(m[10])
        let data = m[11..<(m.count - 1)]
        for (offset, byte) in data.enumerated() { memory[address + UInt32(offset)] = byte }
        status = "Read settings from the RD-88"
    }

    // MARK: Sending

    private func send(_ bytes: [UInt8]) -> Bool {
        let targets = rd88Destinations
        for destination in targets {
            var list = MIDIPacketList()
            let packet = MIDIPacketListInit(&list)
            _ = MIDIPacketListAdd(&list, MemoryLayout<MIDIPacketList>.size, packet, 0, bytes.count, bytes)
            MIDISend(outPort, destination, &list)
        }
        return !targets.isEmpty
    }

    private func rolandMessage(command: UInt8, address: UInt32, body: [UInt8]) -> [UInt8] {
        let addr = [UInt8(address >> 24 & 0x7F), UInt8(address >> 16 & 0x7F), UInt8(address >> 8 & 0x7F), UInt8(address & 0x7F)]
        let sum = (addr + body).reduce(0) { $0 + Int($1) }
        let checksum = UInt8((128 - sum % 128) % 128)
        return [0xF0, 0x41, 0x10, 0x00, 0x00, 0x00, 0x64, command] + addr + body + [checksum, 0xF7]
    }

    /// Asks the keyboard for its System Common and System Control blocks.
    func readSettings() {
        guard send(rolandMessage(command: 0x11, address: Address.systemCommon, body: [0, 0, 0, 0x25])) else {
            status = "No RD-88 found"
            return
        }
        status = "Asked the RD-88 for its settings…"
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
            _ = self.send(self.rolandMessage(command: 0x11, address: Address.systemControl, body: [0, 0, 0, 0x18]))
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
            if self.memory.isEmpty {
                self.status = "The RD-88 did not answer. Check SYSTEM → MIDI → Rx Exclusive is ON."
            }
        }
    }

    private func write(_ address: UInt32, _ bytes: [UInt8]) {
        for (offset, byte) in bytes.enumerated() { memory[address + UInt32(offset)] = byte }
        status = send(rolandMessage(command: 0x12, address: address, body: bytes)) ? "Sent to the RD-88" : "No RD-88 found"
    }

    func sendTestNote() {
        guard send([0x90, 60, 100]) else { status = "No RD-88 destination found"; return }
        status = "Sent middle C to the RD-88"
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { _ = self.send([0x80, 60, 0]) }
    }

    // MARK: Settings

    var settingsLoaded: Bool { memory[Address.localSwitch] != nil && memory[Address.velocityCurve] != nil }

    var localControl: Bool {
        get { (memory[Address.localSwitch] ?? 1) != 0 }
        set { write(Address.localSwitch, [newValue ? 1 : 0]) }
    }

    var velocityCurve: Int {
        get { Int(memory[Address.velocityCurve] ?? 2) }
        set { write(Address.velocityCurve, [UInt8(min(max(newValue, 0), 4))]) }
    }

    var velocityOffset: Int {
        get { Int(memory[Address.velocityOffset] ?? 64) - 64 }
        set { write(Address.velocityOffset, [UInt8(min(max(newValue, -10), 9) + 64)]) }
    }

    /// 0 means the keys send their real velocity; 1...127 is a fixed velocity.
    var fixedVelocity: Int {
        get { Int(memory[Address.velocity] ?? 0) }
        set { write(Address.velocity, [UInt8(min(max(newValue, 0), 127))]) }
    }

    var keyShift: Int {
        get { Int(memory[Address.keyShift] ?? 64) - 64 }
        set { write(Address.keyShift, [UInt8(min(max(newValue, -24), 24) + 64)]) }
    }

    /// Master tune in cents from A = 440 Hz.
    var masterTuneCents: Double {
        get {
            let nibbles = (0..<4).map { UInt32(memory[Address.masterTune + UInt32($0)] ?? [0, 4, 0, 0][$0]) }
            let raw = nibbles[0] << 12 | nibbles[1] << 8 | nibbles[2] << 4 | nibbles[3]
            return (Double(raw) - 1024) / 10
        }
        set {
            let raw = UInt32(min(max((newValue * 10).rounded() + 1024, 24), 2024))
            write(Address.masterTune, [UInt8(raw >> 12 & 0xF), UInt8(raw >> 8 & 0xF), UInt8(raw >> 4 & 0xF), UInt8(raw & 0xF)])
        }
    }
}

struct ContentView: View {
    @StateObject private var link = RD88Link()

    private let curves = ["Super Light", "Light", "Medium", "Heavy", "Super Heavy"]
    private let curveNotes = [
        "Super Light: the easiest to play loud. Full velocity comes with a very gentle touch.",
        "Light: full velocity comes with less force than Medium, so the keys feel lighter.",
        "Medium: the standard response.",
        "Heavy: you have to play harder than Medium to reach full velocity, so the keys feel heavier.",
        "Super Heavy: the hardest to play loud. Full velocity needs a very strong touch.",
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Text("RD-88 Driver").font(.largeTitle).bold()
                Text("This app carries the driver. Switch it on in Settings → RD-88 Driver → Drivers, then plug the RD-88 into the iPad.")
                Button("Open Settings") {
                    if let url = URL(string: UIApplication.openSettingsURLString) {
                        UIApplication.shared.open(url)
                    }
                }
                .buttonStyle(.bordered)
                if !link.status.isEmpty { Text(link.status).foregroundStyle(.secondary) }

                Divider()
                keyboardSettings
                Divider()
                midiCheck
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(32)
        }
    }

    @ViewBuilder private var keyboardSettings: some View {
        Text("Keyboard settings").font(.title2).bold()
        if !link.keyboardConnected {
            Text("Plug in the RD-88 to read and change its settings.").foregroundStyle(.secondary)
        } else {
            Button(link.settingsLoaded ? "Read again from the RD-88" : "Read settings from the RD-88") { link.readSettings() }
                .buttonStyle(.borderedProminent)
        }
        if link.settingsLoaded {
            Toggle("Local control (keys play the RD-88's own sounds)",
                   isOn: Binding(get: { link.localControl }, set: { link.localControl = $0 }))

            Text("Key touch").font(.headline)
            Picker("Key touch", selection: Binding(get: { link.velocityCurve }, set: { link.velocityCurve = $0 })) {
                ForEach(0..<curves.count, id: \.self) { Text(curves[$0]).tag($0) }
            }
            .pickerStyle(.segmented)
            Text(curveNotes[min(max(link.velocityCurve, 0), 4)])
                .font(.footnote).foregroundStyle(.secondary)
            Text("Lighter = loud notes take less force. Heavier = you have to hit harder to get loud.")
                .font(.footnote).foregroundStyle(.secondary)
            Stepper("Key touch offset: \(link.velocityOffset)",
                    value: Binding(get: { link.velocityOffset }, set: { link.velocityOffset = $0 }), in: -10...9)
            Text("Offset is a fine adjustment in between the five steps above.")
                .font(.footnote).foregroundStyle(.secondary)

            Toggle("Fixed velocity (every note the same loudness)",
                   isOn: Binding(get: { link.fixedVelocity > 0 }, set: { link.fixedVelocity = $0 ? 100 : 0 }))
            if link.fixedVelocity > 0 {
                Stepper("Velocity: \(link.fixedVelocity)",
                        value: Binding(get: { link.fixedVelocity }, set: { link.fixedVelocity = max($0, 1) }), in: 1...127)
            }
            Text("Key touch settings apply to scenes that use the system keyboard settings.")
                .font(.footnote).foregroundStyle(.secondary)

            Text("Tuning").font(.headline)
            Stepper("Transpose: \(link.keyShift) semitones",
                    value: Binding(get: { link.keyShift }, set: { link.keyShift = $0 }), in: -24...24)
            Stepper(String(format: "Master tune: %+.1f cents (A = %.1f Hz)", link.masterTuneCents,
                           440 * pow(2, link.masterTuneCents / 1200)),
                    value: Binding(get: { link.masterTuneCents }, set: { link.masterTuneCents = $0 }),
                    in: -100...100, step: 1)
        }
    }

    @ViewBuilder private var midiCheck: some View {
        Text("MIDI check").font(.title2).bold()
        HStack {
            Button("Refresh") { link.refresh() }.buttonStyle(.bordered)
            Button("Send test note to RD-88") { link.sendTestNote() }.buttonStyle(.bordered)
        }

        Text("MIDI inputs the iPad sees").font(.headline)
        if link.sources.isEmpty { Text("None").foregroundStyle(.secondary) }
        ForEach(Array(link.sources.enumerated()), id: \.offset) { _, name in Text("• \(name)") }

        Text("MIDI outputs the iPad sees").font(.headline)
        if link.destinations.isEmpty { Text("None").foregroundStyle(.secondary) }
        ForEach(Array(link.destinations.enumerated()), id: \.offset) { _, name in Text("• \(name)") }

        Text("Incoming messages (newest first)").font(.headline)
        if link.log.isEmpty { Text("Nothing received yet. Play a key on the RD-88.").foregroundStyle(.secondary) }
        ForEach(Array(link.log.enumerated()), id: \.offset) { _, line in
            Text(line).font(.system(.body, design: .monospaced))
        }
    }
}
