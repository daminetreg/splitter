#pragma once
// Declarations of variables another translation unit defines. TODO/47: p4c's indent.h
// declares `static int tabsz;` and indent.cpp defines it; the pieces of indent.cpp never
// read it, log.cpp does.
struct Settings {
    static int tabsz;
};
extern int counter;
