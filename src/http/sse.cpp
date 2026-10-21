#include "zenith/http/sse.hpp"

#include "zenith/http/http_message.hpp"

namespace zenith::http {

// ---------------------------------------------------------------------------
//  ChunkWriter
// ---------------------------------------------------------------------------
ChunkWriter::Ptr ChunkWriter::start(const net::TcpConnection::Ptr& conn, const HttpResponse& head) {
    if (conn == nullptr) return nullptr;
    auto writer = Ptr(new ChunkWriter(conn));
    // 流式响应一律不发 Content-Length；头部写完后连续写分块
    conn->send(head.serialize(/*includeBody=*/false, /*connKeepAlive=*/false));
    return writer;
}

bool ChunkWriter::alive() const noexcept { return conn_ != nullptr && conn_->connected(); }

void ChunkWriter::write(std::string_view data) {
    if (closed_.load() || data.empty() || !alive()) return;
    conn_->send(chunkEncode(data));
}

void ChunkWriter::close() {
    if (closed_.exchange(true)) return;
    if (conn_ != nullptr && conn_->connected()) {
        conn_->send(std::string("0\r\n\r\n"));
        conn_->shutdown();
    }
}

// ---------------------------------------------------------------------------
//  SseStream
// ---------------------------------------------------------------------------
SseStream::Ptr SseStream::start(const net::TcpConnection::Ptr& conn, int status,
                                const Headers& extraHeaders) {
    if (conn == nullptr) return nullptr;
    HttpResponse head;
    head.status = status;
    head.chunked = true;
    head.headers.set("Content-Type", "text/event-stream; charset=utf-8");
    head.headers.set("Cache-Control", "no-cache, no-transform");
    head.headers.set("X-Accel-Buffering", "no"); // nginx 等反代不缓冲
    for (const auto& [name, value] : extraHeaders) head.headers.set(name, value);

    auto writer = ChunkWriter::start(conn, head);
    if (writer == nullptr) return nullptr;
    return Ptr(new SseStream(std::move(writer)));
}

void SseStream::send(std::string_view data, std::string_view event, std::string_view id) {
    if (w_ == nullptr || w_->closed()) return;

    std::string frame;
    frame.reserve(data.size() + event.size() + id.size() + 32);
    if (!event.empty()) {
        frame += "event: ";
        frame.append(event.data(), event.size());
        frame += '\n';
    }
    if (!id.empty()) {
        frame += "id: ";
        frame.append(id.data(), id.size());
        frame += '\n';
    }
    // data 逐行加前缀；末尾换行不额外补空行
    size_t pos = 0;
    for (;;) {
        const size_t nl = data.find('\n', pos);
        std::string_view line =
            (nl == std::string_view::npos) ? data.substr(pos) : data.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        frame += "data: ";
        frame.append(line.data(), line.size());
        frame += '\n';
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
        if (pos >= data.size()) break;
    }
    frame += '\n'; // 事件结束（空行）
    w_->write(frame);
}

void SseStream::retry(int milliseconds) {
    if (w_ == nullptr || w_->closed() || milliseconds < 0) return;
    std::string frame = "retry: ";
    frame += std::to_string(milliseconds);
    frame += "\n\n";
    w_->write(frame);
}

void SseStream::comment(std::string_view text) {
    if (w_ == nullptr || w_->closed()) return;
    std::string frame;
    frame.reserve(text.size() + 4);
    frame += ':';
    if (!text.empty()) {
        frame += ' ';
        frame.append(text.data(), text.size());
    }
    frame += "\n\n";
    w_->write(frame);
}

void SseStream::close() {
    if (w_ != nullptr) w_->close();
}

bool SseStream::closed() const noexcept { return w_ == nullptr || w_->closed(); }

} // namespace zenith::http
