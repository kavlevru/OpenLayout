#include "Board.h"
#include "GLUtils.h"
#include "Utils.h"
#include "THTPad.h"
#include "Circle.h"
#include "Track.h"

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
// A basic single-layer Lee (maze) router. It rasterises existing copper on the
// route layer into an obstacle grid (dilated by one cell for clearance), then
// BFS-routes each pad rubber-band connection on a Manhattan grid, dropping the
// connection and adding a track when it succeeds. It does not rip up, reorder
// nets or use multiple layers, so on dense boards many nets will be left for
// manual routing.

static bool autorouteLayerCopper(uint8_t l) {
	return l == ObjectGroup::LAYER_C1 || l == ObjectGroup::LAYER_C2 ||
	       l == ObjectGroup::LAYER_I1 || l == ObjectGroup::LAYER_I2;
}

static bool autorouteBlocks(const Object *o, uint8_t layer) {
	if(o->GetType() == Object::THT_PAD && ((const THTPad*) o)->HasMetallization())
		return autorouteLayerCopper(layer);
	return o->GetLayer() == layer && autorouteLayerCopper(o->GetLayer());
}

std::pair<int, int> Board::Autoroute(const Settings &settings) {
	uint8_t layer = IsSelectedLayerCopper() ? GetSelectedLayer() : (uint8_t) LAYER_C1;
	float clearance = settings.groundDistance;
	float tw = settings.trackSize;
	float pitch = tw + 2.0f * clearance;
	if(pitch < 0.1f)
		pitch = 0.1f;

	int cols = (int)(size.x / pitch) + 1;
	int rows = (int)(size.y / pitch) + 1;
	if(cols < 2 || rows < 2)
		return {0, 0};

	auto idx    = [&](int c, int r) { return r * cols + c; };
	auto center = [&](int c, int r) { return Vec2((c + 0.5f) * pitch, (r + 0.5f) * pitch); };
	auto clampC = [&](int c) { return c < 0 ? 0 : (c >= cols ? cols - 1 : c); };
	auto clampR = [&](int r) { return r < 0 ? 0 : (r >= rows ? rows - 1 : r); };

	// Base obstacle grid from existing copper on the route layer.
	std::vector<char> base(cols * rows, 0);
	for(Object *o = objects; o; o = o->GetNext()) {
		if(!autorouteBlocks(o, layer))
			continue;
		AABB box = o->GetAABB();
		int c0 = clampC((int)(box.lower.x / pitch) - 1), c1 = clampC((int)(box.upper.x / pitch) + 1);
		int r0 = clampR((int)(box.lower.y / pitch) - 1), r1 = clampR((int)(box.upper.y / pitch) + 1);
		for(int r = r0; r <= r1; r++)
			for(int c = c0; c <= c1; c++)
				if(o->TestPoint(center(c, r)))
					base[idx(c, r)] = 1;
	}
	// Dilate by one cell so routed tracks keep clearance from obstacles.
	std::vector<char> obst(cols * rows, 0);
	for(int r = 0; r < rows; r++)
		for(int c = 0; c < cols; c++)
			if(base[idx(c, r)])
				for(int dr = -1; dr <= 1; dr++)
					for(int dc = -1; dc <= 1; dc++)
						obst[idx(clampC(c + dc), clampR(r + dr))] = 1;

	// Collect unique connection pairs.
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

	int routed = 0, total = pairs.size();
	std::vector<int> prev(cols * rows);
	std::vector<char> seen(cols * rows);

	for(auto &pr : pairs) {
		Pad *a = pr.first, *b = pr.second;

		// Working grid: free the footprints of the two endpoint pads.
		std::vector<char> work = obst;
		for(Pad *pad : {a, b}) {
			AABB box = pad->GetAABB();
			int c0 = clampC((int)(box.lower.x / pitch) - 1), c1 = clampC((int)(box.upper.x / pitch) + 1);
			int r0 = clampR((int)(box.lower.y / pitch) - 1), r1 = clampR((int)(box.upper.y / pitch) + 1);
			for(int r = r0; r <= r1; r++)
				for(int c = c0; c <= c1; c++)
					if(pad->TestPoint(center(c, r)))
						work[idx(c, r)] = 0;
		}

		int sc = clampC((int)(a->GetPosition().x / pitch)), sr = clampR((int)(a->GetPosition().y / pitch));
		int gc = clampC((int)(b->GetPosition().x / pitch)), gr = clampR((int)(b->GetPosition().y / pitch));
		work[idx(sc, sr)] = 0;
		work[idx(gc, gr)] = 0;

		// BFS.
		std::fill(seen.begin(), seen.end(), 0);
		std::vector<int> queue;
		queue.push_back(idx(sc, sr));
		seen[idx(sc, sr)] = 1;
		prev[idx(sc, sr)] = -1;
		size_t head = 0;
		bool found = false;
		const int dc[4] = {1, -1, 0, 0}, dr[4] = {0, 0, 1, -1};
		while(head < queue.size()) {
			int cur = queue[head++];
			if(cur == idx(gc, gr)) { found = true; break; }
			int cc = cur % cols, cr = cur / cols;
			for(int k = 0; k < 4; k++) {
				int nc = cc + dc[k], nr = cr + dr[k];
				if(nc < 0 || nc >= cols || nr < 0 || nr >= rows)
					continue;
				int ni = idx(nc, nr);
				if(seen[ni] || work[ni])
					continue;
				seen[ni] = 1;
				prev[ni] = cur;
				queue.push_back(ni);
			}
		}
		if(!found)
			continue;

		// Backtrace into a cell path (goal -> start), then build the polyline.
		std::vector<int> cells;
		for(int cur = idx(gc, gr); cur != -1; cur = prev[cur])
			cells.push_back(cur);
		std::vector<Vec2> pts;
		pts.push_back(a->GetPosition());
		for(int i = (int)cells.size() - 1; i >= 0; i--)
			pts.push_back(center(cells[i] % cols, cells[i] / cols));
		pts.push_back(b->GetPosition());

		// Drop collinear/duplicate intermediate points.
		std::vector<Vec2> simple;
		for(const Vec2 &p : pts) {
			if(simple.size() >= 2) {
				Vec2 &x = simple[simple.size() - 2], &y = simple[simple.size() - 1];
				if(std::abs(utils::Orientation(x, y, p)) < 1e-4f)
					simple.pop_back();
			}
			if(simple.empty() || (simple.back() - p).LengthSq() > 1e-6f)
				simple.push_back(p);
		}
		if(simple.size() < 2)
			continue;

		AddObjectEnd(new Track(layer, clearance, tw, simple.data(), simple.size()));

		// Mark the routed cells (dilated) as obstacles for later nets.
		for(int cell : cells) {
			int cc = cell % cols, cr = cell / cols;
			for(int ddr = -1; ddr <= 1; ddr++)
				for(int ddc = -1; ddc <= 1; ddc++)
					obst[idx(clampC(cc + ddc), clampR(cr + ddr))] = 1;
		}

		a->RemoveConnections(b);   // rip the rubber-band; removes both directions
		routed++;
	}
	return {routed, total};
}

