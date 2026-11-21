import SwiftUI

@main
struct CabledApp: App {
    @StateObject private var model = CompanionModel()

    var body: some Scene {
        WindowGroup {
            StatusView(model: model)
                .preferredColorScheme(.dark)
        }
    }
}
