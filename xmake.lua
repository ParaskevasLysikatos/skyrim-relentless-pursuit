-- Relentless Pursuit: enemies follow you through load doors and into water, except dragons.
set_xmakever("3.0.0")
set_project("RelentlessPursuit")
set_version("1.2.0")
set_license("MIT")

set_arch("x64")
set_languages("c++23")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")
set_defaultmode("releasedbg")

-- CommonLibSSE-NG is cloned into lib/commonlibsse-ng by CI (see .github/workflows/build.yml).
-- Keep all three runtimes on: that is what the prebuilt CommonLib bundle is compiled for.
set_config("skyrim_se", true)
set_config("skyrim_ae", true)
set_config("skyrim_vr", true)

includes("lib/commonlibsse-ng")

target("RelentlessPursuit", function()
    set_version("1.2.0")
    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "RelentlessPursuit",
        author = "Paraskevas",
        description = "Enemies follow you through load doors and into water, except dragons"
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/PCH.h")
end)
