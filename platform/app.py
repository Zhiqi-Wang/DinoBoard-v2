"""DinoBoard web platform entry point."""
import sys
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles

PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT))

from game_service.routes import router as game_router, replay_router  # noqa: E402
from game_service.pipeline import shutdown_executors  # noqa: E402
from ai_service.routes import router as ai_router  # noqa: E402


@asynccontextmanager
async def lifespan(app: FastAPI):
    yield
    shutdown_executors()


app = FastAPI(title="DinoBoard", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(game_router)
app.include_router(replay_router)
app.include_router(ai_router)

# --- Static file serving ---

static_dir = PROJECT_ROOT / "platform" / "static"
if static_dir.exists():
    app.mount("/static", StaticFiles(directory=str(static_dir)), name="static")

games_dir = PROJECT_ROOT / "games"
for game_dir in sorted(games_dir.iterdir()):
    web_dir = game_dir / "web"
    if web_dir.exists() and (web_dir / "index.html").exists():
        game_name = game_dir.name
        app.mount(
            f"/games/{game_name}",
            StaticFiles(directory=str(web_dir), html=True),
            name=f"game_{game_name}",
        )


@app.get("/")
def index():
    index_path = static_dir / "index.html"
    if index_path.exists():
        return FileResponse(str(index_path))
    return HTMLResponse("<h1>DinoBoard</h1><p>No index.html found.</p>")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
