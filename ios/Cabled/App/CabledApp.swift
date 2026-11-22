import SwiftUI

@main
struct CabledApp: App {
    @StateObject private var model = CompanionModel()

    init() {
        MG.registerFonts()
    }

    var body: some Scene {
        WindowGroup {
            StatusView(model: model)
                .preferredColorScheme(.dark)
        }
    }
}
