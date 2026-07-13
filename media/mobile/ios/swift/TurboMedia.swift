import Foundation

public struct TurboMobileSettings {
    public let targetBitrate: Int
    public let targetFps: Int
    public let resolutionScale: Int
    public let useHardwareCodec: Bool
    public let enableSimulcast: Bool
    public let powerMode: String
}

public final class TurboMobileOptimizer {
    public init() {}

    public func startMonitoring() {
        turbo_battery_monitor_start()
        turbo_network_monitor_start()
    }

    public func stopMonitoring() {
        turbo_network_monitor_stop()
        turbo_battery_monitor_stop()
    }

    public func currentSettings() -> TurboMobileSettings {
        let powerModePointer = turbo_mobile_optimizer_get_power_mode_string()
        let powerMode = powerModePointer.map { String(cString: $0) } ?? "Unknown"

        return TurboMobileSettings(
            targetBitrate: Int(turbo_mobile_optimizer_get_target_bitrate()),
            targetFps: Int(turbo_mobile_optimizer_get_target_fps()),
            resolutionScale: Int(turbo_mobile_optimizer_get_resolution_scale()),
            useHardwareCodec: turbo_mobile_optimizer_should_use_hardware_codec(),
            enableSimulcast: turbo_mobile_optimizer_should_enable_simulcast(),
            powerMode: powerMode
        )
    }
}

@_silgen_name("turbo_battery_monitor_start")
private func turbo_battery_monitor_start()

@_silgen_name("turbo_battery_monitor_stop")
private func turbo_battery_monitor_stop()

@_silgen_name("turbo_network_monitor_start")
private func turbo_network_monitor_start()

@_silgen_name("turbo_network_monitor_stop")
private func turbo_network_monitor_stop()

@_silgen_name("turbo_mobile_optimizer_get_target_bitrate")
private func turbo_mobile_optimizer_get_target_bitrate() -> Int32

@_silgen_name("turbo_mobile_optimizer_get_target_fps")
private func turbo_mobile_optimizer_get_target_fps() -> Int32

@_silgen_name("turbo_mobile_optimizer_get_resolution_scale")
private func turbo_mobile_optimizer_get_resolution_scale() -> Int32

@_silgen_name("turbo_mobile_optimizer_should_use_hardware_codec")
private func turbo_mobile_optimizer_should_use_hardware_codec() -> Bool

@_silgen_name("turbo_mobile_optimizer_should_enable_simulcast")
private func turbo_mobile_optimizer_should_enable_simulcast() -> Bool

@_silgen_name("turbo_mobile_optimizer_get_power_mode_string")
private func turbo_mobile_optimizer_get_power_mode_string() -> UnsafePointer<CChar>?
