import uvicorn
from fastapi import FastAPI
app = FastAPI()
@app.get("/hello")
def hello():
    return "Hello, Benchmark!"
@app.get("/api/users/{uid}")
def user(uid: str):
    return {"id": uid, "name": "zenith"}
uvicorn.run(app, host="127.0.0.1", port=9107, log_level="warning", access_log=False)
