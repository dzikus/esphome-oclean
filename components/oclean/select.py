import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_DEVICE_ID, ENTITY_CATEGORY_CONFIG

from . import (
    CONF_OCLEAN_ID,
    OCLEAN_COMPONENT_SCHEMA,
    apply_entity_prefix,
    hub_name_prefix,
    inject_entity_defaults,
    oclean_ns,
)

DEPENDENCIES = ["oclean"]
CODEOWNERS = ["@dzikus"]

OcleanSchemeSelect = oclean_ns.class_("OcleanSchemeSelect", select.Select, cg.Parented)
OcleanLanguageSelect = oclean_ns.class_(
    "OcleanLanguageSelect", select.Select, cg.Parented
)

CONF_BRUSH_SCHEME = "brush_scheme"
DEFAULT_BRUSH_SCHEME_NAME = "Brushing mode"

CONF_DEVICE_LANGUAGE = "device_language"
DEFAULT_DEVICE_LANGUAGE_NAME = "Display language"

# Language ids in the order the brush firmware numbers them; labels are the
# native names.
LANGUAGES = {
    1: "Chinese (Simplified)",
    2: "Chinese (Traditional)",
    3: "English",
    4: "Francais",
    5: "Japanese",
    6: "Deutsch",
    7: "Russian",
    8: "Espanol",
    9: "Italiano",
    10: "Hebrew",
    11: "Turkce",
    12: "Polski",
    13: "Arabic",
    14: "Korean",
    15: "Portugues",
    16: "Cestina",
    17: "Ukrainian",
}

LANGUAGE_OPTIONS = [LANGUAGES[i] for i in sorted(LANGUAGES)]

# Scheme id -> display name and per-step (gear, duration_s) program. Gears 1-32
# are clean intensity, 33-36 whitening, 37-40 massage, 41 extended. The option
# label appends the total duration, which is the sum of the step durations.
SCHEMES = {
    0: ("Standard Cleaning", [(2, 30), (2, 30), (2, 30), (2, 30)]),
    72: (
        "Strong Cleaning",
        [(16, 30), (16, 30), (24, 30), (16, 30), (16, 30), (37, 30)],
    ),
    73: (
        "Super Cleaning",
        [(38, 30), (24, 30), (24, 30), (24, 30), (24, 30), (32, 30)],
    ),
    74: ("PostWash sensitivity", [(8, 30), (8, 30), (37, 30), (8, 30), (8, 30)]),
    75: ("Standard Whitening", [(34, 30), (34, 30), (24, 30), (16, 30), (16, 30)]),
    76: ("Strong Whitening", [(32, 30), (35, 30), (24, 30), (24, 30), (35, 30)]),
    77: ("Super Whitening", [(32, 30), (36, 30), (32, 30), (36, 30), (32, 30)]),
    78: (
        "Sensitive Cleaning",
        [(37, 30), (41, 30), (41, 30), (41, 30), (41, 30), (41, 30)],
    ),
    79: (
        "Gentle Teeth Spa",
        [(8, 30), (33, 30), (33, 30), (37, 30), (37, 30), (8, 30)],
    ),
    80: (
        "Standard Teeth Spa",
        [(38, 30), (38, 30), (34, 30), (34, 30), (16, 30), (16, 30)],
    ),
    81: (
        "Deep cleaning spa",
        [(39, 30), (35, 30), (24, 30), (24, 30), (39, 30), (35, 30)],
    ),
    82: ("Gum care cleaning", [(18, 30), (18, 30), (12, 30), (12, 30)]),
    83: (
        "Clear your mouth after meals",
        [(41, 30), (41, 30), (1, 30), (41, 30), (41, 30)],
    ),
    84: ("Gum massage", [(38, 30), (38, 30), (38, 30), (38, 30)]),
    85: (
        "Gum Care Cleaning",
        [(8, 30), (8, 30), (37, 30), (8, 30), (8, 30), (37, 20), (8, 30)],
    ),
    86: ("Newbie whitening", [(33, 30), (6, 30), (33, 30), (6, 30), (6, 30)]),
    87: ("Braces Cleaning", [(8, 30), (8, 30), (8, 30), (8, 30), (8, 30), (8, 30)]),
    88: ("Quick cleaning", [(30, 20), (30, 20), (30, 20), (30, 20)]),
    89: ("Travel", [(17, 30), (17, 30), (35, 30), (35, 30)]),
}

# Ids outside the preset range: the firmware accepts and persists arbitrary
# programs there. 120 is the runtime option built from the number entities at
# selection time; 121+ are the named yaml modes, so reordering that list shifts
# ids and changes only how old session records decode.
# Step boundaries drive the brush's pauses, so four steps keeps its quadrant
# guidance intact.
CUSTOM_PNUM = 120
CUSTOM_OPTION_LABEL = "Custom"
CONF_CUSTOM_MODES = "custom_modes"
CONF_NAME = "name"
CONF_PROGRAM = "program"
CONF_GEAR = "gear"
CONF_DURATION = "duration"

# duration is one byte on the wire; presets stay in the 20-30 s range
STEP_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_GEAR): cv.int_range(min=1, max=41),
        cv.Required(CONF_DURATION): cv.int_range(min=5, max=120),
    }
)

CUSTOM_MODE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=64)),
        # Cap at 4 steps: a program over 4 steps exceeds one BLE frame and
        # splits into a 020B continuation, a write path not yet confirmed on
        # Y3P/Y3PD hardware. Single-frame (<=4 steps) is validated empirically.
        # Raise back to 8 once the split-frame write is verified on the brush.
        cv.Required(CONF_PROGRAM): cv.All(
            cv.ensure_list(STEP_SCHEMA), cv.Length(min=1, max=4)
        ),
    }
)


def _format_duration(total_seconds):
    minutes, seconds = divmod(total_seconds, 60)
    return f"{minutes}m{seconds}s"


def _label(name, steps):
    total = sum(duration for _gear, duration in steps)
    return f"{name} ({_format_duration(total)})"


def _scheme_label(pnum):
    name, steps = SCHEMES[pnum]
    return _label(name, steps)


# Option order shown in Home Assistant: scheme id ascending, Standard Cleaning first.
SCHEME_OPTIONS = [_scheme_label(pnum) for pnum in sorted(SCHEMES)]


def _validate_unique_labels(modes):
    # The C++ select resolves an option by linear name match, so two options
    # rendering to the same label make the second one unreachable. Reject
    # collisions among custom modes and against the preset labels.
    preset = set(SCHEME_OPTIONS)
    seen = set()
    for mode in modes:
        steps = [(st[CONF_GEAR], st[CONF_DURATION]) for st in mode[CONF_PROGRAM]]
        label = _label(mode[CONF_NAME], steps)
        if label in preset:
            raise cv.Invalid(
                f"custom_modes entry {label!r} collides with a preset "
                f"brushing-mode label; rename it or change its duration"
            )
        if label in seen:
            raise cv.Invalid(
                f"custom_modes has two entries that render as {label!r}; "
                f"option labels must be unique"
            )
        seen.add(label)
    return modes


_DEFAULT_NAMES = [
    (CONF_BRUSH_SCHEME, DEFAULT_BRUSH_SCHEME_NAME),
    (CONF_DEVICE_LANGUAGE, DEFAULT_DEVICE_LANGUAGE_NAME),
]


def _inject_defaults(config):
    return inject_entity_defaults(config, _DEFAULT_NAMES)


CONFIG_SCHEMA = cv.All(
    _inject_defaults,
    OCLEAN_COMPONENT_SCHEMA.extend(
        {
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            cv.Optional(CONF_BRUSH_SCHEME): select.select_schema(
                OcleanSchemeSelect,
                icon="mdi:toothbrush",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ).extend(
                {
                    cv.Optional(CONF_CUSTOM_MODES): cv.All(
                        cv.ensure_list(CUSTOM_MODE_SCHEMA),
                        cv.Length(max=20),
                        _validate_unique_labels,
                    ),
                }
            ),
            cv.Optional(CONF_DEVICE_LANGUAGE): select.select_schema(
                OcleanLanguageSelect,
                icon="mdi:translate",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
        }
    ),
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_OCLEAN_ID])
    config = apply_entity_prefix(
        config, _DEFAULT_NAMES, hub_name_prefix(config[CONF_OCLEAN_ID])
    )

    sub = config.get(CONF_BRUSH_SCHEME)
    if sub is not None:
        # Named yaml modes get ids right after the runtime custom id, in list order.
        modes = []
        for i, mode in enumerate(sub.get(CONF_CUSTOM_MODES, [])):
            steps = [(st[CONF_GEAR], st[CONF_DURATION]) for st in mode[CONF_PROGRAM]]
            modes.append((CUSTOM_PNUM + 1 + i, _label(mode[CONF_NAME], steps), steps))
        options = (
            SCHEME_OPTIONS
            + [label for _pnum, label, _steps in modes]
            + [CUSTOM_OPTION_LABEL]
        )
        sel = await select.new_select(sub, options=options)
        await cg.register_parented(sel, hub)
        cg.add(hub.set_scheme_select(sel))
        for pnum in sorted(SCHEMES):
            _name, steps = SCHEMES[pnum]
            flat = [v for gear_dur in steps for v in gear_dur]
            cg.add(sel.add_scheme(pnum, _scheme_label(pnum), flat))
        for pnum, label, steps in modes:
            flat = [v for gear_dur in steps for v in gear_dur]
            cg.add(sel.add_scheme(pnum, label, flat))
        cg.add(sel.set_custom_option(CUSTOM_PNUM, CUSTOM_OPTION_LABEL))

    sub = config.get(CONF_DEVICE_LANGUAGE)
    if sub is not None:
        sel = await select.new_select(sub, options=LANGUAGE_OPTIONS)
        await cg.register_parented(sel, hub)
        cg.add(hub.set_language_select(sel))
        for lang_id in sorted(LANGUAGES):
            cg.add(sel.add_language(lang_id, LANGUAGES[lang_id]))
