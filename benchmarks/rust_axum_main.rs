use axum::{routing::get, Json, Router, extract::Path};
use std::net::SocketAddr;

#[tokio::main]
async fn main() {
    let app = Router::new()
        .route("/hello", get(|| async { "Hello, Benchmark!" }))
        .route("/api/users/:id", get(|Path(id): Path<String>| async move {
            Json(serde_json::json!({"id": id, "name": "zenith"}))
        }));
    let addr = SocketAddr::from(([127, 0, 0, 1], 9108));
    println!("axum on 9108");
    let listener = tokio::net::TcpListener::bind(addr).await.unwrap();
    axum::serve(listener, app).await.unwrap();
}
