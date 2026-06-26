#include "Board.h"
#include "GLUtils.h"
#include "Utils.h"
#include "THTPad.h"
#include "Circle.h"
#include "Track.h"
#include <queue>
#include <algorithm>
#include <functional>
#include <cfloat>
#include <cmath>

Board::Board(const char *_name, Type type, Vec2 innerSize, float border, bool originTop) : Board() {
	objects = nullptr;
	if(type != Type::Empty)
		size.Set(innerSize.x + border * 2.0f, innerSize.y + border * 2.0f);
	else
		size = innerSize;

	if(type == Type::Round) {
		Circle *circle = new Circle(LAYER_O, 0.0f, 0.0f, innerSize * 0.5f + Vec2(border, border), innerSize.x, 0.0f, 0.0f);
		AddObjectBegin(circle);
	} else if(type == Type::Rectangle) {
		const Vec2 points[] = {
			{border, border},
			{border + innerSize.x, border},
			{border + innerSize.x, border + innerSize.y},
			{border, border + innerSize.y},
			{border, border}
		};
		Track *frame = new Track(LAYER_O, 0.0f, 0.0f, points, 5);
		AddObjectBegin(frame);
	}
	if(originTop)
		origin = Vec2(border, border);
	else
		origin = Vec2(border, size.y - border);
	strcpy(name, _name);
	for(int i = 0; i < 7; i++) {
		groundPane[i] = false;
		layerVisible[i] = true;
	}
	multilayer = false;
	activeLayer = LAYER_C2;
	activeGrid = grid = 1.27;
	camera.Set(0.0f, 0.0f);
	zoom = 10;
	origin.Set(0.0f, 0.0f);
}

void Board::Save(File &file) const {
	file.WriteString(name, 30);
	file.WriteNull(4);
	size.SaveInt(file);
	file.Write(groundPane, 7);
	file.Write<double>(grid * 10000.0f);
	file.Write<double>(zoom / 10000.0f);
	file.Write<int32_t>(camera.x * zoom);
	file.Write<int32_t>(camera.y * zoom);
	file.Write<uint32_t>(activeLayer + 1);
	file.Write(layerVisible, 7);
	images.Save(file);
	file.WriteNull(8);
	origin.InvY().SaveInt(file);
	file.Write<uint8_t>(multilayer);

	file.Write<uint32_t>(GetObjectCount());
	for(Object *object = objects; object; object = object->GetNext())
		object->Save(file);
	for(Object *object = objects; object; object = object->GetNext())
		object->SaveConnections(objects, file);
}


void Board::Load(File &file) {
	file.ReadString(name, 30);
	file.ReadNull(4);
	size.LoadInt(file);
	file.Read(groundPane, 7);
	grid = file.Read<double>() / 10000.0f;
	zoom = file.Read<double>() * 10000.0f;
	camera.x = file.Read<int32_t>() / zoom;
	camera.y = file.Read<int32_t>() / zoom;
	activeLayer = file.Read<uint32_t>() - 1;
	file.Read(layerVisible, 7);
	images.Load(file);
	file.ReadNull(8);
	origin.LoadInt(file);
	origin = origin.InvY();
	multilayer = file.Read<uint8_t>();

	uint32_t objectCount = file.Read<uint32_t>();
	Object *last = nullptr;
	for(int i = 0; i < objectCount; i++) {
		Object *object = Object::Load(file);
		if(!object)
			break;          // unknown type: stream is misaligned, stop
		last = AddObjectEnd(object, last);
	}
	for(Object *object = objects; object; object = object->GetNext())
		object->LoadConnections(objects, file);

	UpdateGrid(false, false);
}

bool Board::IsSelectedLayerCopper() const {
	return	activeLayer == LAYER_C1 || activeLayer == LAYER_C2 ||
			activeLayer == LAYER_I1 || activeLayer == LAYER_I2;
}

void Board::UpdateGrid(bool shift, bool ctrl) {
	if(ctrl)
		activeGrid = 0.0;
	else if(shift)
		activeGrid = grid * 0.5;
	else
		activeGrid = grid;
}

Vec2 Board::ToGrid(const Vec2 &vec) const {
	return utils::ToGrid(vec, grid, origin);
}

Vec2 Board::ToActiveGrid(const Vec2 &vec) const {
	if(activeGrid == 0.0)
		return vec;
	return utils::ToGrid(vec, activeGrid, origin);
}

Vec2 Board::ToActiveGrid(const Vec2 &vec, const Vec2 &customOrigin) const {
	if(activeGrid == 0.0)
		return vec;
	return utils::ToGrid(vec, activeGrid, customOrigin);
}

void Board::SnapSelectedToGrid() {
	for(Object *object = objects; object; object = object->GetNext())
		if(object->IsSelected() && object->groups.Empty())
			object->Move(ToGrid(object->GetPosition()) - object->GetPosition());
	bool ok = true;
	for(int i = 0; ok; i++) {
		ok = false;
		Vec2 delta;
		for(Object *object = objects; object; object = object->GetNext())
			if(object->IsSelected() && !object->groups.Empty() && object->groups.Last() == i) {
				if(!ok)
					delta = ToGrid(object->GetPosition()) - object->GetPosition();
				object->Move(delta);
				ok = true;
			}
	}
}

Object *Board::TestPoint(const Vec2 &point) {
	for(Object *object = objects; object; object = object->GetNext())
		if(object->IsPad() && object->GetAABB().TestPoint(point) &&
                object->TestPoint(point) && layerVisible[object->GetLayer()])
			return object;
	for(Object *object = objects; object; object = object->GetNext())
		if(!object->IsPad() && object->GetAABB().TestPoint(point) &&
                object->TestPoint(point) && layerVisible[object->GetLayer()])
			return object;
	return nullptr;
}

void Board::UpdateCamera(const Vec2 &delta) {
	camera -= delta;
}

void Board::SaveView() {
	viewHistory.push_back({camera, zoom});
	if(viewHistory.size() > 64)
		viewHistory.erase(viewHistory.begin());
}
void Board::ZoomPrevious() {
	if(viewHistory.empty())
		return;
	View v = viewHistory.back();
	viewHistory.pop_back();
	camera = v.camera;
	zoom = v.zoom;
}
void Board::Zoom(float ratio, const Vec2 &mouse) {
	SaveView();
	// pos = mouse / zoom + camera
	// pos1 = pos2
	// mouse / zoom1 + camera1 = mouse / zoom2 + camera2
	// camera2 = camera1 + mouse / zoom1 - mouse / zoom2
	// camera2 = camera1 + mouse * (1 / zoom1 - 1 / zoom2)
	// camera2 = camera1 + mouse * (zoom2 - zoom1) / (zoom1 * zoom2)
	// camera2 = camera1 + mouse * zoom1 * (ratio - 1) / (zoom1 * zoom1 * ratio)
	// camera2 = camera1 + mouse * (ratio - 1) / (zoom1 * ratio)
	camera += mouse * (ratio - 1.0f) / (zoom * ratio);
	zoom *= ratio;
}

void Board::ZoomAABB(const Vec2 &screenSize, const AABB &aabb) {
	SaveView();
	Vec2 size = aabb.Size();
	camera = aabb.lower;
	if(screenSize.x / screenSize.y > size.x / size.y) {
		zoom = screenSize.y / size.y;
		camera.x -= (screenSize.x / screenSize.y * size.y - size.x) / 2.0f;
	} else {
		zoom = screenSize.x / size.x;
		camera.y -= (screenSize.y / screenSize.x * size.x - size.y) / 2.0f;
	}
}

void Board::ZoomBoard(const Vec2 &screenSize) {
	ZoomAABB(screenSize, AABB(Vec2(0.0f, 0.0f), size));
}

void Board::ZoomObjects(const Vec2 &screenSize) {
	if(!objects)
		return;
	AABB aabb(objects->GetAABB());
	for(Object *object = objects->GetNext(); object; object = object->GetNext())
		aabb |= object->GetAABB();
	ZoomAABB(screenSize, aabb);
}

void Board::ZoomSelection(const Vec2 &screenSize) {
	Object *first = GetFirstSelected();
	if(!first)
		return;
	AABB aabb(first->GetAABB());
	for(Object *object = objects->GetNext(); object; object = object->GetNext())
		if(object->IsSelected())
			aabb |= object->GetAABB();
	ZoomAABB(screenSize, aabb);
}

Vec2 Board::ConvertToCoords(const Vec2 &vec) const {
	return (vec / zoom + camera);
}
Vec2 Board::ConvertFromCoords(const Vec2 &vec) const {
	return ((vec - camera) * zoom);
}
void Board::Draw(const Settings &settings, const Vec2 &screenSize) const {
	const ColorScheme &colors = settings.GetColorScheme();
	glMatrixMode(GL_PROJECTION);

	if(settings.selectedTool == TOOL_PHOTOVIEW) {
		// Photo-realistic preview: dark surround, FR4 substrate, opaque copper /
		// silk / outline, holes in dark. No grid, ground planes or selection.
		glClearColor(0.13f, 0.13f, 0.13f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glViewport(0, 0, screenSize.x, screenSize.y);
		glLoadIdentity();
		glOrtho(0.0f, screenSize.x / zoom, screenSize.y / zoom, 0.0f, 0.0f, 1.0f);
		glutils::Translate(-camera);

		glColor3ub(12, 105, 55);                  // FR4 substrate
		glRectf(0.0f, 0.0f, size.x, size.y);

		glEnable(GL_SCISSOR_TEST);
		glScissor(-camera.x * zoom, screenSize.y - (size.y - camera.y) * zoom, size.x * zoom, size.y * zoom);

		images.Draw();
		DrawObjectsPhoto(activeLayer, layerVisible);

		glColor3ub(10, 10, 10);                   // drill holes
		DrawDrillings(layerVisible);

		glDisable(GL_SCISSOR_TEST);
		return;
	}

	glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glViewport(0, 0, screenSize.x, screenSize.y);
	glLoadIdentity();
	glOrtho(0.0f, screenSize.x / zoom, screenSize.y / zoom, 0.0f, 0.0f, 1.0f);
	glutils::Translate(-camera);

	if(GetCurrentLayerGround()) {
		if(settings.darkGround)
			colors.SetGroundColor(COLOR_C1 + activeLayer);
		else
			colors.SetColor(COLOR_C1 + activeLayer);
	} else
		colors.SetColor(COLOR_BGR);

	glRectf(0.0f, 0.0f, size.x, size.y);

	glEnable(GL_SCISSOR_TEST);
	glScissor(-camera.x * zoom, screenSize.y - (size.y - camera.y) * zoom, size.x * zoom, size.y * zoom);

	images.Draw();

	if(GetCurrentLayerGround()) {
		colors.SetColor(COLOR_BGR);
		DrawGroundDistance(activeLayer);
	}

	if(settings.showGrid && activeGrid * zoom > 6.0)
		DrawGrid(settings, screenSize);

	if(settings.transparent) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
	}
	DrawObjects(colors, activeLayer, false, layerVisible);
	glDisable(GL_BLEND);

	colors.SetColor(COLOR_SELO);
	DrawSelected();

	colors.SetDrillingsColor(settings.drill);
	DrawDrillings(layerVisible);

	colors.SetColor(COLOR_CON);
	DrawConnections();

	if(settings.selectedTool == TOOL_SOLDER_MASK) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		DrawSoldermaskMarked();
		glDisable(GL_BLEND);
	}

	glDisable(GL_SCISSOR_TEST);
}

void Board::DrawGrid(const Settings &settings, const Vec2 &screenSize) const {
	glEnable(GL_POINT_SMOOTH);
	glDisable(GL_LINE_SMOOTH);
	const ColorScheme &colors = settings.GetColorScheme();
	uint8_t subgrid = settings.GetSubGrid();
	double subgridValue = subgrid * activeGrid;
	Vec2 begin(fmod(origin.x, subgridValue) - subgridValue, fmod(origin.y, subgridValue) - subgridValue);
	Vec2 end(size.x, size.y);
	if(settings.gridStyle == GRID_LINES) {
		colors.SetColor(COLOR_LINES);
		glLineWidth(1.0f);
		glBegin(GL_LINES);
		for(double x = begin.x; x < end.x; x += activeGrid) {
			glutils::Vertex(Vec2(x, begin.y));
			glutils::Vertex(Vec2(x, end.y));
		}
		for(double y = begin.y; y < end.y; y += activeGrid) {
			glutils::Vertex(Vec2(begin.x, y));
			glutils::Vertex(Vec2(end.x, y));
		}
		glEnd();
		if(subgrid != 1) {
			glLineWidth(2.0f);
			glBegin(GL_LINES);
			for(double x = begin.x; x < end.x; x += subgridValue) {
				glutils::Vertex(Vec2(x, begin.y));
				glutils::Vertex(Vec2(x, end.y));
			}
			for(double y = begin.y; y < end.y; y += subgridValue) {
				glutils::Vertex(Vec2(begin.x, y));
				glutils::Vertex(Vec2(end.x, y));
			}
			glEnd();
		}
	} else {
		colors.SetColor(COLOR_DOTS);
		glPointSize(1.0f);
		glBegin(GL_POINTS);
		for(double x = begin.x; x < end.x; x += activeGrid)
			for(double y = begin.y; y < end.y; y += activeGrid)
				glutils::Vertex(Vec2(x, y));
		glEnd();
		if(subgrid != 1) {
			glPointSize(3.0f);
			glBegin(GL_POINTS);
			for(double x = begin.x; x < end.x; x += subgridValue)
				for(double y = begin.y; y < end.y; y += subgridValue)
					glutils::Vertex(Vec2(x, y));
			glEnd();
		}
	}
}

void Board::DrawConnections() const {
	glLineWidth(1.5f);
	glBegin(GL_LINES);
	for(const Object *object = objects; object; object = object->GetNext()) 
		object->DrawConnections();
	glEnd();
}

void Board::DrawSelected() const {
	for(const Object *object = objects; object; object = object->GetNext())
		if(object->IsSelected())
			object->DrawObject();
}

// --- Autorouter -------------------------------------------------------------
//
// A two-layer Lee/Dijkstra (maze) router. Existing copper on the top (C1) and
// bottom (C2) sides is rasterised into per-side obstacle grids, dilated by one
// cell for clearance. Each pad rubber-band connection is routed on a Manhattan
// grid with a turn penalty (straighter tracks) and a via cost for switching
// sides; vias are dropped as metallised through-pads where a cell is free on
// both sides. Shorter nets route first. There is no rip-up/reroute, so dense
// boards may still leave some nets for manual routing.

// side 0 = top copper (C1), side 1 = bottom copper (C2)
static uint8_t autorouteSideLayer(int side) {
	return side == 0 ? ObjectGroup::LAYER_C1 : ObjectGroup::LAYER_C2;
}

static bool autorouteBlocks(const Object *o, int side) {
	if(o->GetType() == Object::THT_PAD)
		return true;                                  // through-pad blocks both sides
	return o->GetLayer() == autorouteSideLayer(side);
}

// Sides a pad can be routed on: bit0 = top, bit1 = bottom.
static uint8_t autoroutePadSides(const Pad *p) {
	if(p->GetType() == Object::THT_PAD)
		return 0x3;
	uint8_t l = p->GetLayer();
	if(l == ObjectGroup::LAYER_C1) return 0x1;
	if(l == ObjectGroup::LAYER_C2) return 0x2;
	return 0;
}

std::pair<int, int> Board::Autoroute(const Settings &settings) {
	float clearance = settings.groundDistance;
	float tw = settings.trackSize;
	float minPitch = tw + 2.0f * clearance;       // spacing needed to keep clearance
	if(minPitch < 0.1f)
		minPitch = 0.1f;
	// Prefer the board grid as the routing pitch: pads sit on it, so grid nodes
	// land on pad centres and tracks run through them. Only fall back to the
	// minimum spacing if the board grid is finer than that.
	float pitch = (float) GetGrid();
	if(pitch < minPitch)
		pitch = minPitch;

	// Collect unique connection pairs first — the first pad anchors the routing
	// grid so its nodes fall on pad centres (tracks then pass through the
	// centres instead of running offset beside them).
	std::vector<std::pair<Pad*, Pad*>> pairs;
	for(Object *o = objects; o; o = o->GetNext()) {
		if(!o->IsPad())
			continue;
		Pad *p = (Pad*) o;
		for(uint32_t i = 0; i < p->ConnectionCount(); i++) {
			Pad *q = p->GetConnection(i);
			if(p < q)
				pairs.push_back({p, q});
		}
	}
	if(pairs.empty())
		return {0, 0};

	Vec2 ref = pairs[0].first->GetPosition();
	Vec2 off(std::fmod(ref.x, pitch), std::fmod(ref.y, pitch));   // grid node alignment

	auto node   = [&](int c, int r) { return Vec2(off.x + c * pitch, off.y + r * pitch); };
	auto cellX  = [&](float x) { return (int) std::lround((x - off.x) / pitch); };
	auto cellY  = [&](float y) { return (int) std::lround((y - off.y) / pitch); };
	int cols = cellX(size.x) + 2;
	int rows = cellY(size.y) + 2;
	if(cols < 2 || rows < 2)
		return {0, 0};

	auto idx    = [&](int c, int r) { return r * cols + c; };
	auto clampC = [&](int c) { return c < 0 ? 0 : (c >= cols ? cols - 1 : c); };
	auto clampR = [&](int r) { return r < 0 ? 0 : (r >= rows ? rows - 1 : r); };
	auto blockCell = [&](std::vector<char> &g, int cell) {
		int cc = cell % cols, cr = cell / cols;
		for(int ddr = -1; ddr <= 1; ddr++)
			for(int ddc = -1; ddc <= 1; ddc++)
				g[idx(clampC(cc + ddc), clampR(cr + ddr))] = 1;
	};

	// Per-side obstacle grids (dilated for clearance): side 0 = top, 1 = bottom.
	std::vector<char> obst[2] = {std::vector<char>(cols * rows, 0),
	                             std::vector<char>(cols * rows, 0)};
	for(int side = 0; side < 2; side++) {
		std::vector<char> base(cols * rows, 0);
		for(Object *o = objects; o; o = o->GetNext()) {
			if(!autorouteBlocks(o, side))
				continue;
			AABB box = o->GetAABB();
			int c0 = clampC(cellX(box.lower.x) - 1), c1 = clampC(cellX(box.upper.x) + 1);
			int r0 = clampR(cellY(box.lower.y) - 1), r1 = clampR(cellY(box.upper.y) + 1);
			float h = pitch * 0.5f;
			for(int r = r0; r <= r1; r++)
				for(int c = c0; c <= c1; c++) {
					Vec2 n = node(c, r);
					// Sample a 3x3 grid inside the cell so thin obstacles (a track
					// passing between node columns) are not missed.
					bool hit = false;
					for(int sy = -1; sy <= 1 && !hit; sy++)
						for(int sx = -1; sx <= 1 && !hit; sx++)
							if(o->TestPoint(n + Vec2(sx * h, sy * h)))
								hit = true;
					if(hit)
						base[idx(c, r)] = 1;
				}
		}
		for(int r = 0; r < rows; r++)
			for(int c = 0; c < cols; c++)
				if(base[idx(c, r)])
					blockCell(obst[side], idx(c, r));
	}

	// Route short connections first — long nets otherwise block many later ones.
	std::sort(pairs.begin(), pairs.end(),
		[](const std::pair<Pad*, Pad*> &x, const std::pair<Pad*, Pad*> &y) {
			Vec2 dx = x.first->GetPosition() - x.second->GetPosition();
			Vec2 dy = y.first->GetPosition() - y.second->GetPosition();
			return std::abs(dx.x) + std::abs(dx.y) < std::abs(dy.x) + std::abs(dy.y);
		});

	int routed = 0, total = pairs.size();
	const int dc[4] = {1, -1, 0, 0}, dr[4] = {0, 0, 1, -1};
	const float bend = 2.0f;                      // turn penalty, in cell steps
	const float viaCost = 12.0f;                  // cost of switching sides
	// state = (cell * 2 + side) * 5 + incoming dir (4 = none)
	int nStates = cols * rows * 2 * 5;
	std::vector<float> dist(nStates);
	std::vector<int> prev(nStates);

	auto simplify = [&](const std::vector<Vec2> &in) {
		std::vector<Vec2> s;
		for(const Vec2 &p : in) {
			if(s.size() >= 2) {
				Vec2 &x = s[s.size() - 2], &y = s[s.size() - 1];
				if(std::abs(utils::Orientation(x, y, p)) < 1e-4f)
					s.pop_back();
			}
			if(s.empty() || (s.back() - p).LengthSq() > 1e-6f)
				s.push_back(p);
		}
		return s;
	};

	for(auto &pr : pairs) {
		Pad *a = pr.first, *b = pr.second;
		uint8_t startSides = autoroutePadSides(a), goalSides = autoroutePadSides(b);
		if(!startSides || !goalSides)
			continue;

		// Working grids: undo exactly what each endpoint pad contributed to the
		// base grid — its 3x3-sampled cells AND their dilation ring — so the
		// track can leave its own pad. (A plain radius missed the outer dilated
		// ring and trapped the route inside the pad.)
		std::vector<char> work[2] = {obst[0], obst[1]};
		for(Pad *pad : {a, b}) {
			AABB box = pad->GetAABB();
			int c0 = clampC(cellX(box.lower.x) - 1), c1 = clampC(cellX(box.upper.x) + 1);
			int r0 = clampR(cellY(box.lower.y) - 1), r1 = clampR(cellY(box.upper.y) + 1);
			float h = pitch * 0.5f;
			for(int r = r0; r <= r1; r++)
				for(int c = c0; c <= c1; c++) {
					Vec2 n = node(c, r);
					bool hit = false;
					for(int sy = -1; sy <= 1 && !hit; sy++)
						for(int sx = -1; sx <= 1 && !hit; sx++)
							if(pad->TestPoint(n + Vec2(sx * h, sy * h)))
								hit = true;
					if(hit)
						for(int ddr = -1; ddr <= 1; ddr++)
							for(int ddc = -1; ddc <= 1; ddc++) {
								work[0][idx(clampC(c + ddc), clampR(r + ddr))] = 0;
								work[1][idx(clampC(c + ddc), clampR(r + ddr))] = 0;
							}
				}
		}

		int sc = clampC(cellX(a->GetPosition().x)), sr = clampR(cellY(a->GetPosition().y));
		int gc = clampC(cellX(b->GetPosition().x)), gr = clampR(cellY(b->GetPosition().y));
		int startCell = idx(sc, sr), goalCell = idx(gc, gr);
		work[0][startCell] = work[1][startCell] = 0;
		work[0][goalCell]  = work[1][goalCell]  = 0;

		auto stateOf = [&](int cell, int side, int dir) { return (cell * 2 + side) * 5 + dir; };

		std::fill(dist.begin(), dist.end(), FLT_MAX);
		std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>,
		                    std::greater<std::pair<float, int>>> pq;
		for(int side = 0; side < 2; side++)
			if((startSides >> side) & 1) {
				int st = stateOf(startCell, side, 4);
				dist[st] = 0.0f;
				prev[st] = -1;
				pq.push({0.0f, st});
			}
		int goalState = -1;
		while(!pq.empty()) {
			float cost = pq.top().first;
			int state = pq.top().second;
			pq.pop();
			if(cost > dist[state])
				continue;
			int dir = state % 5, tmp = state / 5, side = tmp % 2, cell = tmp / 2;
			if(cell == goalCell && ((goalSides >> side) & 1)) { goalState = state; break; }
			int cc = cell % cols, cr = cell / cols;
			for(int k = 0; k < 4; k++) {            // planar moves on the current side
				int nc = cc + dc[k], nr = cr + dr[k];
				if(nc < 0 || nc >= cols || nr < 0 || nr >= rows)
					continue;
				int ncell = idx(nc, nr);
				if(work[side][ncell])
					continue;
				float ncost = cost + 1.0f + ((dir != 4 && dir != k) ? bend : 0.0f);
				int nstate = stateOf(ncell, side, k);
				if(ncost < dist[nstate]) {
					dist[nstate] = ncost;
					prev[nstate] = state;
					pq.push({ncost, nstate});
				}
			}
			if(!work[0][cell] && !work[1][cell]) {  // via: switch side if free on both
				int nstate = stateOf(cell, 1 - side, 4);
				float ncost = cost + viaCost;
				if(ncost < dist[nstate]) {
					dist[nstate] = ncost;
					prev[nstate] = state;
					pq.push({ncost, nstate});
				}
			}
		}
		if(goalState < 0)
			continue;

		// Backtrace into a (cell, side) path, start -> goal.
		std::vector<std::pair<int, int>> path;       // {cell, side}
		for(int s = goalState; s != -1; s = prev[s])
			path.push_back({(s / 5) / 2, (s / 5) % 2});
		std::reverse(path.begin(), path.end());

		// Emit one track per same-side run, with a via pad at each side change.
		std::vector<Vec2> viaPositions;
		bool madeTrack = false, first = true;
		size_t i = 0;
		while(i < path.size()) {
			int side = path[i].second;
			std::vector<Vec2> run;
			size_t j = i;
			while(j < path.size() && path[j].second == side) {
				run.push_back(node(path[j].first % cols, path[j].first / cols));
				j++;
			}
			if(first)
				run.insert(run.begin(), a->GetPosition());
			bool last = (j >= path.size());
			if(last)
				run.push_back(b->GetPosition());
			std::vector<Vec2> simple = simplify(run);
			if(simple.size() >= 2) {
				AddObjectEnd(new Track(autorouteSideLayer(side), clearance, tw,
				                       simple.data(), simple.size()));
				madeTrack = true;
			}
			if(!last)
				viaPositions.push_back(node(path[j].first % cols, path[j].first / cols));
			first = false;
			i = j;
		}
		if(!madeTrack)
			continue;

		for(const Vec2 &vp : viaPositions)
			AddObjectEnd(new THTPad((uint8_t) LAYER_C1, clearance, vp,
			                        settings.padSize, settings.padShape, true));

		// Mark routed cells (dilated) as obstacles on their side for later nets.
		for(auto &cs : path)
			blockCell(obst[cs.second], cs.first);

		a->RemoveConnections(b);   // rip the rubber-band; removes both directions
		routed++;
	}
	return {routed, total};
}

