#include "zenith/http/context.hpp"

#include "zenith/http/session.hpp"

namespace zenith::http {

std::string Context::cookie(const std::string& name, const std::string& def) const {
    // Cookie: a=1; b=2（值 urlDecode）
    const std::string raw = req_.headers.get("Cookie");
    if (raw.empty()) return def;
    for (const std::string& pairStr : core::string_util::split(raw, ';', false)) {
        const std::string t = core::string_util::trim(pairStr);
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (core::string_util::trim(t.substr(0, eq)) == name) {
            return urlDecode(core::string_util::trim(t.substr(eq + 1)), false);
        }
    }
    return def;
}

const std::vector<multipart::FormPart>& Context::formParts() const {
    if (formCache_ == nullptr) {
        formCache_ = std::make_shared<std::vector<multipart::FormPart>>();
        const std::string ct = req_.headers.get("Content-Type");
        if (ct.compare(0, 19, "multipart/form-data") == 0) {
            auto b = multipart::extractBoundary(ct);
            if (b) {
                auto parts = multipart::parse(req_.body, b.value());
                if (parts) *formCache_ = std::move(parts).value();
            }
        }
    }
    return *formCache_;
}

std::string Context::form(const std::string& name) const {
    for (const auto& p : formParts()) {
        if (p.name == name && !p.isFile()) return p.data;
    }
    return "";
}

const multipart::FormPart* Context::file(const std::string& name) const {
    for (const auto& p : formParts()) {
        if (p.name == name && p.isFile()) return &p;
    }
    return nullptr;
}

std::shared_ptr<Session> Context::session() const {
    return std::static_pointer_cast<Session>(context_);
}

} // namespace zenith::http
