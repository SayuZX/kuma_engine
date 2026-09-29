// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package com.flutter.gradle

import com.android.build.api.dsl.ApplicationExtension
import org.gradle.api.GradleException
import org.gradle.api.Project
import java.io.File
import java.util.zip.ZipFile

/** Packages the signed native asset resolver during a normal Android release build. */
internal object PackedFlutterAssets {
    private val abis = mapOf(
        "armeabi-v7a" to Pair("android_release_lto", "arm"),
        "arm64-v8a" to Pair("android_release_arm64_lto", "arm64"),
    )

    fun enabled(project: Project): Boolean =
        project.findProperty("flutter.customPackedAssets")?.toString()?.toBoolean() == true

    fun configure(project: Project, flutterRoot: File, android: ApplicationExtension) {
        // Flutter's default universal APK also contains x86_64. Only hardened
        // ARM engines exist for this configuration.
        android.packaging.jniLibs.excludes.addAll(setOf("lib/x86/**", "lib/x86_64/**"))
        project.tasks.matching { it.name == "mergeReleaseNativeLibs" }.configureEach {
            dependsOn("compileFlutterBuildRelease")
            outputs.upToDateWhen { false }
            doLast { stage(project, flutterRoot) }
        }
        // FlutterPlugin's assemble doLast copies this checked APK to flutter-apk.
        project.tasks.matching { it.name == "assembleRelease" }.configureEach {
            doFirst { verifyApks(project) }
        }
    }

    private fun run(command: List<String>, environment: Map<String, String> = emptyMap()) {
        val builder = ProcessBuilder(command).redirectErrorStream(true)
        builder.environment().putAll(environment)
        val process = builder.start()
        val output = process.inputStream.bufferedReader().use { it.readText() }
        if (process.waitFor() != 0) {
            throw GradleException("Packed Flutter asset tool failed: ${command.first()}\n$output")
        }
        if (output.isNotBlank()) {
            println(output.trim())
        }
    }

    private fun stage(project: Project, flutterRoot: File) {
        val engineSrc = File(flutterRoot, "engine/src")
        val custom = File(flutterRoot, "custom-engine")
        val build = project.layout.buildDirectory.get().asFile
        val assets = File(build, "intermediates/flutter/release/flutter_assets")
        val packed = File(build, "intermediates/kuma_packed/release")
        val nativeDir = File(build, "intermediates/merged_native_libs/release/mergeReleaseNativeLibs/out/lib")
        val keyDir = File(System.getProperty("user.home"), ".config/kuma-engine")
        val signer = File(engineSrc, "out/host_debug_unopt/asset_signer")
        val header = File(engineSrc, "flutter/assets/packed_asset_public_key_generated.h")
        val publicKey = File(keyDir, "payload-public.ed25519")
        val privateKey = File(keyDir, "payload-private.ed25519")
        val dart = File(flutterRoot, "bin/cache/dart-sdk/bin/dart")
        val packer = File(custom, "asset_packer/flutter_asset_packer.dart")
        val linker = File(custom, "linker/build_libpayload.sh")
        val strip = File(engineSrc, "flutter/buildtools/mac-x64/clang/bin/llvm-strip")
        val inputs = listOf(signer, header, publicKey, privateKey, dart, packer, linker, strip)
        if (!assets.isDirectory || inputs.any { !it.isFile }) {
            throw GradleException("Packed Flutter engine inputs are missing")
        }
        packed.mkdirs()
        val exportedHeader = File(packed, "public_key.h")
        run(listOf(signer.path, "export-header", "--public", publicKey.path,
            "--output", exportedHeader.path))
        if (!exportedHeader.readBytes().contentEquals(header.readBytes())) {
            throw GradleException("Payload public key differs from the custom Flutter engine")
        }
        run(listOf(dart.path, packer.path, "--input", assets.path, "--output", packed.path,
            "--compression", "auto", "--hash", "fnv1a", "--alignment", "16"))
        val signedPayload = File(packed, "payload-signed.bin")
        run(listOf(signer.path, "sign", "--input", File(packed, "payload.bin").path,
            "--output", signedPayload.path, "--private", privateKey.path, "--alignment", "16"))
        run(listOf(signer.path, "verify", "--input", signedPayload.path,
            "--public", publicKey.path))

        for ((abi, info) in abis) {
            val out = File(engineSrc, "out/${info.first}")
            val engine = File(out, "libflutter.so")
            val args = File(out, "args.gn")
            if (!engine.isFile || !args.isFile ||
                !args.readText().contains("target_cpu = \"${info.second}\"") ||
                !args.readText().contains("flutter_custom_asset_hardened = true") ||
                engine.lastModified() < header.lastModified()) {
                throw GradleException("Hardened custom Flutter engine is missing/stale for $abi")
            }
            val destination = File(nativeDir, abi)
            destination.mkdirs()
            run(listOf(strip.path, "--strip-unneeded", "-o",
                File(destination, "libflutter.so").path, engine.path))
            run(listOf("bash", linker.path, signedPayload.path,
                File(destination, "libpayload.so").path, abi),
                mapOf("ENGINE_SRC" to engineSrc.path))
        }
    }

    private fun verifyApks(project: Project) {
        val apkDir = File(project.layout.buildDirectory.get().asFile, "outputs/apk/release")
        val split = project.findProperty("split-per-abi")?.toString()?.toBoolean() == true
        val names = if (split) {
            listOf("app-armeabi-v7a-release.apk", "app-arm64-v8a-release.apk")
        } else {
            listOf("app-release.apk")
        }
        for (name in names) {
            val apk = File(apkDir, name)
            if (!apk.isFile) throw GradleException("Release APK missing: $name")
            ZipFile(apk).use { archive ->
                val entries = archive.entries().asSequence().map { it.name }.toSet()
                if (entries.any { it.startsWith("assets/flutter_assets/") ||
                        it.startsWith("assets/dolby/") || it.endsWith(".apk") }) {
                    throw GradleException("Unpacked asset remains in $name")
                }
                val actual = entries.filter { it.startsWith("lib/") && it.endsWith("/libflutter.so") }
                    .map { it.split('/')[1] }.toSet()
                val expected = if (split) {
                    setOf(name.removePrefix("app-").removeSuffix("-release.apk"))
                } else {
                    abis.keys
                }
                if (actual != expected || expected.any { "lib/$it/libpayload.so" !in entries }) {
                    throw GradleException("Packed ABI mismatch in $name: $actual")
                }
            }
        }
    }
}
