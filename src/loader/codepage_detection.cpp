#include "loader/codepage_detection.h"

#include <algorithm>
#include <array>
#include <span>
#include <unordered_set>

namespace gm {
namespace {

template <usize N>
struct WordModel {
    std::array<u16, 2 * N> characters;
    std::array<u32, N> words;
};

template <usize N>
consteval auto make_model(const u32 (&words)[N]) {
    WordModel<N> model{};
    for (usize i = 0; i < N; ++i) {
        model.characters[2 * i] = static_cast<u16>(words[i] >> 16);
        model.characters[2 * i + 1] = static_cast<u16>(words[i]);
        model.words[i] = words[i];
    }
    std::ranges::sort(model.characters);
    std::ranges::sort(model.words);
    return model;
}

#include "loader/codepage_words.inc"

struct Candidate {
    u32 cp;
    std::span<const u16> characters;
    std::span<const u32> words;
};

constexpr Candidate candidates[] = {
    {932, kJapanese.characters, kJapanese.words},
    {936, kSimplified.characters, kSimplified.words},
    {950, kTraditional.characters, kTraditional.words},
    {949, kKorean.characters, kKorean.words},
    {1252, {}, {}},
};

constexpr bool between(int n, int lo, int hi) { return n >= lo && n <= hi; }

bool valid_pair(u32 cp, u8 a, u8 b) {
    if (cp == 932) {
        return (between(a, 0x81, 0x9F) || between(a, 0xE0, 0xFC)) &&
               between(b, 0x40, 0xFC) && b != 0x7F;
    }
    if (!between(a, 0x81, 0xFE)) return false;
    if (cp == 950) return between(b, 0x40, 0x7E) || between(b, 0xA1, 0xFE);
    if (cp == 949) {
        return between(b, 0xA1, 0xFE) ||
               (a <= 0xC6 && (between(b, 'A', 'Z') || between(b, 'a', 'z') || between(b, 0x81, 0xA0)));
    }
    return between(b, 0x40, 0xFE) && b != 0x7F;
}

int score_text(std::string_view text, const Candidate& candidate) {
    int score = 0;
    u32 previous = 0;
    for (usize i = 0; i < text.size();) {
        const u8 a = static_cast<u8>(text[i++]);
        if (a < 128) {
            previous = 0;
            continue;
        }
        const u8 b = i < text.size() ? static_cast<u8>(text[i]) : 0;
        if (candidate.cp == 1252) {
            // An isolated accent in an otherwise Latin word is useful Western evidence.
            const u8 left = i > 1 ? static_cast<u8>(text[i - 2]) : 0;
            const auto letter = [](u8 c) { return between(c, 'A', 'Z') || between(c, 'a', 'z'); };
            if (left < 128 && b < 128 && (letter(left) || letter(b))) score += 5;
            else score -= 2;
            continue;
        }
        if (candidate.cp == 932 && between(a, 0xA1, 0xDF)) {
            // A run of half-width kana with a length or voicing mark is stronger
            // evidence than isolated kana, which overlap ordinary GBK bytes.
            if (between(previous, 0xA6, 0xDD)) score += a == 0xB0 || a == 0xDE || a == 0xDF ? 5 : 1;
            previous = a;
            continue;
        }
        if (!valid_pair(candidate.cp, a, b)) {
            score -= 8;
            previous = 0;
            continue;
        }
        ++i;
        const u16 pair = static_cast<u16>(a * 256 + b);
        if (std::ranges::binary_search(candidate.characters, pair)) score += 5;
        if (previous && std::ranges::binary_search(candidate.words, (previous << 16) | pair)) score += 10;
        previous = pair;
        if (candidate.cp == 932 && (between(pair, 0x829F, 0x82F1) || between(pair, 0x8340, 0x8396))) score += 5;
        if (candidate.cp == 936 && between(a, 0xB0, 0xD7) && between(b, 0xA1, 0xFE)) ++score;
        if (candidate.cp == 950 && between(a, 0xA4, 0xC6)) ++score;
        if (candidate.cp == 949 && between(a, 0xB0, 0xC8) && between(b, 0xA1, 0xFE)) ++score;
    }
    return score;
}

struct Evidence {
    explicit Evidence(usize budget) : remaining(budget) {}
    std::array<int, std::size(candidates)> scores{};
    std::unordered_set<std::string_view> seen;
    usize remaining;

    void add(std::string_view text) {
        if (remaining == 0) return;
        const auto first = std::ranges::find_if(text, [](u8 c) { return c >= 128; });
        if (first == text.end()) return;
        const usize offset = static_cast<usize>(first - text.begin());
        text = text.substr(offset ? offset - 1 : 0, std::min<usize>(remaining, 512));
        if (!seen.insert(text).second) return;
        remaining -= text.size();
        for (usize i = 0; i < scores.size(); ++i) scores[i] += score_text(text, candidates[i]);
    }

    GameEncoding decide(u32 font, std::string_view source) const {
        if (seen.empty()) return {};
        std::array<usize, std::size(candidates)> order{0, 1, 2, 3, 4};
        std::ranges::stable_sort(order, [&](usize a, usize b) { return scores[a] > scores[b]; });
        const usize best = order[0];
        const int margin = scores[best] - scores[order[1]];
        const bool conflicts_with_font = font && candidates[best].cp != font;
        if (scores[best] >= (conflicts_with_font ? 12 : 4) &&
            margin >= std::max(conflicts_with_font ? 8 : 3, scores[best] / 10)) {
            return {EncodingMode::LegacyAnsi, candidates[best].cp, source};
        }
        if (font) return {EncodingMode::LegacyAnsi, font, "font charset"};
        if (scores[0] >= scores[best] - 3 && scores[0] >= 0 && scores[4] < scores[0]) {
            return {EncodingMode::LegacyAnsi, 932, "Japanese prior (ambiguous text)"};
        }
        // A close GBK/Big5 match can still rule out both Japanese and Western text.
        if (scores[best] > 0 && scores[best] > scores[4]) {
            return {EncodingMode::LegacyAnsi, candidates[best].cp, "ambiguous text"};
        }
        return {};
    }
};

struct Sampler {
    usize scan_left = 256 * 1024;
    Evidence literals{7 * 1024};
    Evidence comments{1024};

    bool done() const { return scan_left == 0 || literals.remaining == 0; }

    void field(std::string_view text, bool gml) {
        // Charge even empty fields and cap individual scripts so one resource cannot take the entire budget.
        text = text.substr(0, std::min<usize>(scan_left, 16 * 1024));
        scan_left -= std::min(scan_left, std::max<usize>(1, text.size()));
        if (!gml) {
            literals.add(text);
            return;
        }
        for (usize i = 0; i < text.size() && literals.remaining;) {
            const char c = text[i++];
            if (c == '"' || c == '\'') {
                const usize end = std::min(text.size(), text.find(c, i));
                literals.add(text.substr(i, end - i));
                i = end + 1;
            } else if (c == '/' && i < text.size() && (text[i] == '/' || text[i] == '*')) {
                const bool block = text[i++] == '*';
                const usize end = std::min(text.size(), block ? text.find("*/", i) : text.find_first_of("\r\n", i));
                comments.add(text.substr(i, end - i));
                i = end + (block ? 2 : 1);
            }
        }
    }

    void event(const EventData& event) {
        for (const auto& a : event.actions) {
            if (done()) break;
            if (a.kind == 7 && !a.args.empty()) { // Execute Code action
                field(a.args[0], true);
                continue;
            }
            field(a.code, true);
            for (int i = 0; i < a.arg_count && static_cast<usize>(i) < a.args.size() && !done(); ++i) {
                const int type = static_cast<usize>(i) < a.arg_types.size() ? a.arg_types[i] : 0;
                const bool string_argument = type == 1 || type == 2 || type == 15; // string, string expression, font string
                field(a.args[i], !string_argument);
            }
        }
    }

    void collect(const GameData& game) {
        field(game.help.caption, false);
        for (const auto& room : game.rooms) {
            if (done()) return;
            field(room.caption, false);
        }
        for (const auto& constant : game.constants) {
            if (done()) return;
            field(constant.value, true);
        }
        // Reserve at least half of the scan for object/room events in script-heavy games.
        const usize reserved = scan_left / 2;
        scan_left -= reserved;
        for (const auto& script : game.scripts) {
            if (done()) break;
            field(script.source, true);
        }
        scan_left += reserved;
        for (const auto& obj : game.objects) {
            for (const auto& group : obj.events) {
                for (const auto& entry : group) {
                    if (done()) return;
                    event(entry.event);
                }
            }
        }
        for (const auto& timeline : game.timelines) {
            for (const auto& moment : timeline.moments) {
                if (done()) return;
                event(moment.event);
            }
        }
        for (const auto& room : game.rooms) {
            if (done()) return;
            field(room.creation_code, true);
            for (const auto& inst : room.instances) {
                if (done()) return;
                field(inst.creation_code, true);
            }
        }
    }
};

} // namespace

GameEncoding select_game_encoding(const GameData& game, u32 override_cp) {
    if (game.meta.detected_version != VersionMode::GM80) return {EncodingMode::Utf8, 65001, "GM version"};
    if (override_cp) return {EncodingMode::LegacyAnsi, override_cp, "--codepage"};

    u32 font = 0;
    for (const auto& f : game.fonts) {
        if (!f.exists) continue;
        const auto cp = gdi_charset_to_codepage(f.charset);
        if (cp != 1252) {
            font = cp;
            break;
        }
    }
    Sampler samples;
    samples.collect(game);
    if (!samples.literals.seen.empty()) return samples.literals.decide(font, "game text");
    if (font) return {EncodingMode::LegacyAnsi, font, "font charset"};
    return samples.comments.decide(0, "code comments");
}

} // namespace gm
