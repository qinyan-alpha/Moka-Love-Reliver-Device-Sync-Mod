#!/usr/bin/env python3
"""Extract the Live2D motion parameters used for SR6 synchronization.

The generated header is self-contained; UnityPy is only needed when regenerating
it after a game update, not when building or running the mod.
"""

from __future__ import annotations

import argparse
import collections
import math
import os
import struct
from dataclasses import dataclass

import UnityPy


PARAM_PISTON_MAIN = 2584966004       # Parameters/ParamPistonMain
PARAM_PINSTON_ANGLE = 3565076246     # Parameters/ParamPinstonAngle (shipped typo)
PARAM_BODY_ANGLE_UD3 = 73079845      # Parameters/ParamBodyAngleUD3
PARAM_BODY_ANGLE_Y = 855789717       # Parameters/ParamBodyAngleY
PARAM_BODY_ANGLE_Z = 2852847919      # Parameters/ParamBodyAngleZ
PARAM_BODY_ANGLE_UD = 2404401299     # Parameters/ParamBodyAngleUD

PARAMETER_NAMES = {
    PARAM_PISTON_MAIN: "ParamPistonMain",
    PARAM_PINSTON_ANGLE: "ParamPinstonAngle",
    PARAM_BODY_ANGLE_UD3: "ParamBodyAngleUD3",
    PARAM_BODY_ANGLE_Y: "ParamBodyAngleY",
    PARAM_BODY_ANGLE_Z: "ParamBodyAngleZ",
    PARAM_BODY_ANGLE_UD: "ParamBodyAngleUD",
}


@dataclass
class ParameterCurve:
    path_hash: int
    name: str
    minimum: float
    maximum: float
    keys: list[tuple[float, float, float, float, float]]


@dataclass
class Motion:
    episode: int
    motion_id: int
    name: str
    duration: float
    parameters: list[ParameterCurve]


def u32_to_float(value: int) -> float:
    return struct.unpack("<f", struct.pack("<I", value))[0]


def parse_streamed_clip(data: list[int]) -> dict[int, list[tuple[float, float, float, float, float]]]:
    curves: dict[int, list[tuple[float, float, float, float, float]]] = collections.defaultdict(list)
    offset = 0
    while offset < len(data):
        time = u32_to_float(data[offset])
        key_count = data[offset + 1]
        offset += 2
        if key_count > 10000 or offset + key_count * 5 > len(data):
            raise ValueError(f"invalid StreamedClip frame at word {offset}: key_count={key_count}")
        for _ in range(key_count):
            curve_index = data[offset]
            coefficients = tuple(u32_to_float(x) for x in data[offset + 1 : offset + 5])
            curves[curve_index].append((time, *coefficients))
            offset += 5
    return curves


def evaluate(keys: list[tuple[float, float, float, float, float]], time: float) -> float:
    lo = 0
    hi = len(keys)
    while lo < hi:
        mid = (lo + hi) // 2
        if keys[mid][0] <= time:
            lo = mid + 1
        else:
            hi = mid
    key = keys[max(0, lo - 1)]
    dt = max(0.0, time - key[0])
    return ((key[1] * dt + key[2]) * dt + key[3]) * dt + key[4]


def aligned4(value: int) -> int:
    return (value + 3) & ~3


def read_motion_database(asset_path: str, database_path_id: int, episode: int) -> list[Motion]:
    env = UnityPy.load(asset_path)
    objects = {obj.path_id: obj for obj in env.objects}
    database = objects[database_path_id].get_raw_data()
    asset_count = struct.unpack_from("<I", database, 48)[0]

    animation_clips = {
        obj.path_id: obj.read()
        for obj in env.objects
        if obj.type.name == "AnimationClip"
    }

    primary_hash = PARAM_PISTON_MAIN if episode == 1 else PARAM_BODY_ANGLE_UD3
    motions: list[Motion] = []

    for index in range(asset_count):
        _, asset_id = struct.unpack_from("<Iq", database, 52 + index * 12)
        asset_obj = objects[asset_id]
        asset_name = asset_obj.peek_name()

        is_target = (
            episode == 1
            and asset_name.startswith("Layer1_piston")
        ) or (
            episode == 2
            and asset_name.startswith("Layer1_")
            and "_main" in asset_name
        )
        if not is_target:
            continue

        raw = asset_obj.get_raw_data()
        name_length = struct.unpack_from("<I", raw, 28)[0]
        data_offset = aligned4(32 + name_length)
        layer, motion_id = struct.unpack_from("<ii", raw, data_offset)
        _, loop_clip_id = struct.unpack_from("<Iq", raw, data_offset + 20)
        if layer != 0 or not loop_clip_id or loop_clip_id not in animation_clips:
            continue

        clip = animation_clips[loop_clip_id]
        bindings = clip.m_ClipBindingConstant.genericBindings
        streamed = parse_streamed_clip(clip.m_MuscleClip.m_Clip.data.m_StreamedClip.data)
        duration = float(clip.m_MuscleClip.m_StopTime)
        parameters: list[ParameterCurve] = []

        for binding_index, binding in enumerate(bindings):
            path_hash = int(binding.path)
            if path_hash not in PARAMETER_NAMES:
                continue
            keys = [key for key in streamed.get(binding_index, []) if key[0] > -1.0e20]
            if not keys:
                continue
            samples = [
                evaluate(keys, min(duration, sample / 120.0))
                for sample in range(int(math.ceil(duration * 120.0)) + 1)
            ]
            minimum = min(samples)
            maximum = max(samples)
            if maximum - minimum < 1.0e-5:
                continue
            parameters.append(ParameterCurve(
                path_hash,
                PARAMETER_NAMES[path_hash],
                minimum,
                maximum,
                keys,
            ))

        parameter_hashes = {parameter.path_hash for parameter in parameters}
        has_primary = primary_hash in parameter_hashes
        if episode == 2 and not has_primary:
            has_primary = PARAM_BODY_ANGLE_Y in parameter_hashes
        if not has_primary:
            continue

        motions.append(Motion(episode, motion_id, clip.m_Name, duration, parameters))

    return sorted(motions, key=lambda motion: (motion.episode, motion.motion_id))


def f32(value: float) -> str:
    if not math.isfinite(value):
        raise ValueError(f"non-finite float: {value}")
    text = f"{value:.9g}"
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


def write_header(path: str, motions: list[Motion]) -> None:
    lines = [
        "#pragma once",
        "// Generated by tools/generate_motion_curves.py from the shipped Unity assets.",
        "// Only body/piston parameters are retained; face, hair, hand and physics curves are excluded.",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "inline constexpr std::uint32_t kParamPistonMain = 2584966004u;",
        "inline constexpr std::uint32_t kParamPinstonAngle = 3565076246u;",
        "inline constexpr std::uint32_t kParamBodyAngleUD3 = 73079845u;",
        "inline constexpr std::uint32_t kParamBodyAngleY = 855789717u;",
        "inline constexpr std::uint32_t kParamBodyAngleZ = 2852847919u;",
        "inline constexpr std::uint32_t kParamBodyAngleUD = 2404401299u;",
        "",
        "struct Live2DCurveKey { float time, a, b, c, d; };",
        "struct Live2DParameterCurve {",
        "    std::uint32_t pathHash;",
        "    const char* parameterName;",
        "    float minimum;",
        "    float maximum;",
        "    const Live2DCurveKey* keys;",
        "    std::size_t keyCount;",
        "};",
        "struct Live2DMotionCurve {",
        "    int episode;",
        "    int motionId;",
        "    const char* clipName;",
        "    float duration;",
        "    const Live2DParameterCurve* parameters;",
        "    std::size_t parameterCount;",
        "};",
        "",
    ]

    for motion_index, motion in enumerate(motions):
        for parameter_index, parameter in enumerate(motion.parameters):
            lines.append(f"inline constexpr Live2DCurveKey kLive2DKeys_{motion_index}_{parameter_index}[] = {{")
            for key in parameter.keys:
                lines.append("    { " + ", ".join(f32(value) for value in key) + " },")
            lines.append("};")
            lines.append("")

        lines.append(f"inline constexpr Live2DParameterCurve kLive2DParameters_{motion_index}[] = {{")
        for parameter_index, parameter in enumerate(motion.parameters):
            escaped_name = parameter.name.replace("\\", "\\\\").replace('"', '\\"')
            lines.append(
                f'    {{ {parameter.path_hash}u, "{escaped_name}", {f32(parameter.minimum)}, {f32(parameter.maximum)}, '
                f"kLive2DKeys_{motion_index}_{parameter_index}, "
                f"sizeof(kLive2DKeys_{motion_index}_{parameter_index}) / sizeof(kLive2DKeys_{motion_index}_{parameter_index}[0]) }},"
            )
        lines.append("};")
        lines.append("")

    lines.append("inline constexpr Live2DMotionCurve kLive2DMotionCurves[] = {")
    for motion_index, motion in enumerate(motions):
        escaped_name = motion.name.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(
            f'    {{ {motion.episode}, {motion.motion_id}, "{escaped_name}", {f32(motion.duration)}, '
            f"kLive2DParameters_{motion_index}, "
            f"sizeof(kLive2DParameters_{motion_index}) / sizeof(kLive2DParameters_{motion_index}[0]) }},"
        )
    lines.extend([
        "};",
        "",
        "inline const Live2DMotionCurve* FindLive2DMotionCurve(int episode, int motionId)",
        "{",
        "    for (const auto& motion : kLive2DMotionCurves)",
        "        if (motion.episode == episode && motion.motionId == motionId)",
        "            return &motion;",
        "    return nullptr;",
        "}",
        "",
        "inline const Live2DParameterCurve* FindLive2DParameterCurve(",
        "    const Live2DMotionCurve& motion, std::uint32_t pathHash)",
        "{",
        "    for (std::size_t index = 0; index < motion.parameterCount; ++index)",
        "        if (motion.parameters[index].pathHash == pathHash)",
        "            return &motion.parameters[index];",
        "    return nullptr;",
        "}",
        "",
    ])

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as output:
        output.write("\n".join(lines))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-dir", required=True, help="MokaLoveRelive game directory")
    parser.add_argument("--output", default=os.path.join("src", "generated_motion_curves.h"))
    args = parser.parse_args()

    data_dir = os.path.join(args.game_dir, "MocaLoveRelive_Data")
    motions = []
    motions.extend(read_motion_database(os.path.join(data_dir, "sharedassets0.assets"), 922, 1))
    motions.extend(read_motion_database(os.path.join(data_dir, "sharedassets1.assets"), 912, 2))
    write_header(args.output, motions)
    parameter_count = sum(len(motion.parameters) for motion in motions)
    print(f"generated {len(motions)} motions / {parameter_count} body parameters -> {args.output}")


if __name__ == "__main__":
    main()
