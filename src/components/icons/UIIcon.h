#pragma once

// Icon identifiers used across the UI.
//
// Kept in a standalone header so non-theme code (e.g. the app registry that
// describes which icon a desktop app uses) can reference an icon without
// pulling in BaseTheme, GfxRenderer or the settings singleton.
enum UIIcon { None = 0, Folder, Text, Image, Book, File, Recent, Settings, Transfer, Library, Wifi, Hotspot, Bookmark };
