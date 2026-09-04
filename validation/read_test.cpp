// Reads roundtrip_out.dxf and roundtrip_out.dwg back and prints every
// field of every entity we wrote, so we can confirm the round trip is
// byte-for-byte faithful (within floating point tolerance), not just
// "didn't crash."

#include <cstdio>
#include <string>

#include "libdxfrw.h"
#include "libdwgr.h"
#include "drw_entities.h"
#include "drw_objects.h"
#include "drw_header.h"

namespace {

class ReadDriver : public DRW_Interface {
public:
    void addHeader(const DRW_Header *) override {}
    void addLType(const DRW_LType &) override {}
    void addLayer(const DRW_Layer &data) override {
        std::printf("LAYER: name='%s' color=%d\n", data.name.c_str(), data.color);
    }
    void addDimStyle(const DRW_Dimstyle &) override {}
    void addVport(const DRW_Vport &) override {}
    void addTextStyle(const DRW_Textstyle &) override {}
    void addAppId(const DRW_AppId &) override {}
    void addBlock(const DRW_Block &data) override {
        std::printf("BLOCK: name='%s' base=(%.3f,%.3f)\n", data.name.c_str(),
                    data.basePoint.x, data.basePoint.y);
    }
    void setBlock(int) override {}
    void endBlock() override {}
    void addPoint(const DRW_Point &) override {}
    void addLine(const DRW_Line &data) override {
        std::printf("LINE: (%.3f,%.3f) -> (%.3f,%.3f) layer='%s'\n",
                    data.basePoint.x, data.basePoint.y,
                    data.secPoint.x, data.secPoint.y, data.layer.c_str());
    }
    void addRay(const DRW_Ray &) override {}
    void addXline(const DRW_Xline &) override {}
    void addArc(const DRW_Arc &data) override {
        std::printf("ARC: center=(%.3f,%.3f) r=%.3f start=%.3f end=%.3f layer='%s'\n",
                    data.basePoint.x, data.basePoint.y, data.radious,
                    data.staangle, data.endangle, data.layer.c_str());
    }
    void addCircle(const DRW_Circle &data) override {
        std::printf("CIRCLE: center=(%.3f,%.3f) r=%.3f layer='%s'\n",
                    data.basePoint.x, data.basePoint.y, data.radious, data.layer.c_str());
    }
    void addEllipse(const DRW_Ellipse &) override {}
    void addLWPolyline(const DRW_LWPolyline &data) override {
        std::printf("LWPOLYLINE: %zu verts closed=%d layer='%s' [",
                    data.vertlist.size(), (data.flags & 1) != 0, data.layer.c_str());
        for (const auto &v : data.vertlist) std::printf("(%.3f,%.3f) ", v->x, v->y);
        std::printf("]\n");
    }
    void addPolyline(const DRW_Polyline &) override {}
    void addSpline(const DRW_Spline *) override {}
    void addKnot(const DRW_Entity &) override {}
    void addInsert(const DRW_Insert &data) override {
        std::printf("INSERT: block='%s' at=(%.3f,%.3f) layer='%s'\n",
                    data.name.c_str(), data.basePoint.x, data.basePoint.y, data.layer.c_str());
    }
    void addTrace(const DRW_Trace &) override {}
    void add3dFace(const DRW_3Dface &) override {}
    void addSolid(const DRW_Solid &) override {}
    void addMText(const DRW_MText &) override {}
    void addText(const DRW_Text &data) override {
        std::printf("TEXT: '%s' at=(%.3f,%.3f) height=%.3f layer='%s'\n",
                    data.text.c_str(), data.basePoint.x, data.basePoint.y,
                    data.height, data.layer.c_str());
    }
    void addDimAlign(const DRW_DimAligned *) override {}
    void addDimLinear(const DRW_DimLinear *) override {}
    void addDimRadial(const DRW_DimRadial *) override {}
    void addDimDiametric(const DRW_DimDiametric *) override {}
    void addDimAngular(const DRW_DimAngular *) override {}
    void addDimAngular3P(const DRW_DimAngular3p *) override {}
    void addDimOrdinate(const DRW_DimOrdinate *) override {}
    void addDimArc(const DRW_DimArc *) override {}
    void addLeader(const DRW_Leader *) override {}
    void addHatch(const DRW_Hatch *) override {}
    void addViewport(const DRW_Viewport &) override {}
    void addImage(const DRW_Image *) override {}
    void linkImage(const DRW_ImageDef *) override {}
    void addComment(const char *) override {}
    void addPlotSettings(const DRW_PlotSettings *) override {}
    void writeHeader(DRW_Header &) override {}
    void writeBlocks() override {}
    void writeBlockRecords() override {}
    void writeEntities() override {}
    void writeLTypes() override {}
    void writeLayers() override {}
    void writeTextstyles() override {}
    void writeVports() override {}
    void writeDimstyles() override {}
    void writeObjects() override {}
    void writeAppId() override {}
};

} // namespace

int main() {
    std::printf("===== Reading roundtrip_out.dxf =====\n");
    {
        ReadDriver drv;
        dxfRW reader("roundtrip_out.dxf");
        bool ok = reader.read(&drv, false);
        std::printf("DXF read: %s\n", ok ? "OK" : "FAILED");
    }

    std::printf("\n===== Reading roundtrip_out.dwg =====\n");
    {
        ReadDriver drv;
        dwgRW reader("roundtrip_out.dwg");
        bool ok = reader.read(&drv, false);
        std::printf("DWG read: %s (error=%d)\n", ok ? "OK" : "FAILED",
                    static_cast<int>(reader.getError()));
    }

    return 0;
}
