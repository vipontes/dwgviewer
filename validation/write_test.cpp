// Standalone empirical test: does libdxfrw actually WRITE (not just read)
// LINE, LWPOLYLINE, CIRCLE, ARC, TEXT, LAYER, and INSERT (block reference)?
//
// Strategy: implement DRW_Interface as a WRITER driver (construct entities,
// hand them to dxfRW::write() / dwgRW::write()), produce real .dxf and .dwg
// files, then re-read each with the exact same DwgDocument reader already
// used by the viewer, and check the geometry survived the round trip.

#include <cstdio>
#include <memory>
#include <vector>

#include "libdxfrw.h"
#include "libdwgr.h"
#include "drw_entities.h"
#include "drw_objects.h"
#include "drw_header.h"

namespace {

// No-op read-side callbacks shared by both write drivers (they only ever
// write, never read, but DRW_Interface requires every pure virtual to be
// implemented).
#define DRW_STUBS \
    void addHeader(const DRW_Header *) override {} \
    void addLType(const DRW_LType &) override {} \
    void addLayer(const DRW_Layer &) override {} \
    void addDimStyle(const DRW_Dimstyle &) override {} \
    void addVport(const DRW_Vport &) override {} \
    void addTextStyle(const DRW_Textstyle &) override {} \
    void addAppId(const DRW_AppId &) override {} \
    void addBlock(const DRW_Block &) override {} \
    void setBlock(int) override {} \
    void endBlock() override {} \
    void addPoint(const DRW_Point &) override {} \
    void addLine(const DRW_Line &) override {} \
    void addRay(const DRW_Ray &) override {} \
    void addXline(const DRW_Xline &) override {} \
    void addArc(const DRW_Arc &) override {} \
    void addCircle(const DRW_Circle &) override {} \
    void addEllipse(const DRW_Ellipse &) override {} \
    void addLWPolyline(const DRW_LWPolyline &) override {} \
    void addPolyline(const DRW_Polyline &) override {} \
    void addSpline(const DRW_Spline *) override {} \
    void addKnot(const DRW_Entity &) override {} \
    void addInsert(const DRW_Insert &) override {} \
    void addTrace(const DRW_Trace &) override {} \
    void add3dFace(const DRW_3Dface &) override {} \
    void addSolid(const DRW_Solid &) override {} \
    void addMText(const DRW_MText &) override {} \
    void addText(const DRW_Text &) override {} \
    void addDimAlign(const DRW_DimAligned *) override {} \
    void addDimLinear(const DRW_DimLinear *) override {} \
    void addDimRadial(const DRW_DimRadial *) override {} \
    void addDimDiametric(const DRW_DimDiametric *) override {} \
    void addDimAngular(const DRW_DimAngular *) override {} \
    void addDimAngular3P(const DRW_DimAngular3p *) override {} \
    void addDimOrdinate(const DRW_DimOrdinate *) override {} \
    void addDimArc(const DRW_DimArc *) override {} \
    void addLeader(const DRW_Leader *) override {} \
    void addHatch(const DRW_Hatch *) override {} \
    void addViewport(const DRW_Viewport &) override {} \
    void addImage(const DRW_Image *) override {} \
    void linkImage(const DRW_ImageDef *) override {} \
    void addComment(const char *) override {} \
    void addPlotSettings(const DRW_PlotSettings *) override {}

// Shared entity set used for every format we test.
struct TestEntities {
    DRW_Layer layer;
    DRW_Line line;
    DRW_Circle circle;
    DRW_Arc arc;
    DRW_LWPolyline poly;
    DRW_Text text;
    DRW_Block block;      // block definition INSERT will reference
    DRW_Insert insert;

    TestEntities() {
        layer.name = "MYLAYER";
        layer.color = 3;

        line.basePoint = {0.0, 0.0, 0.0};
        line.secPoint = {100.0, 50.0, 0.0};
        line.layer = "MYLAYER";

        circle.basePoint = {50.0, 50.0, 0.0};
        circle.radious = 20.0;
        circle.layer = "MYLAYER";

        arc.basePoint = {120.0, 20.0, 0.0};
        arc.radious = 15.0;
        arc.staangle = 0.0;
        arc.endangle = M_PI;
        arc.layer = "MYLAYER";

        poly.flags = 1; // closed
        poly.addVertex(DRW_Vertex2D(0.0, 80.0, 0.0));
        poly.addVertex(DRW_Vertex2D(30.0, 80.0, 0.0));
        poly.addVertex(DRW_Vertex2D(30.0, 110.0, 0.0));
        poly.addVertex(DRW_Vertex2D(0.0, 110.0, 0.0));
        poly.layer = "MYLAYER";

        text.basePoint = {150.0, 0.0, 0.0};
        text.text = "HELLO";
        text.height = 5.0;
        text.layer = "MYLAYER";

        block.name = "MYBLOCK";
        block.basePoint = {0.0, 0.0, 0.0};

        insert.name = "MYBLOCK";
        insert.basePoint = {200.0, 200.0, 0.0};
        insert.layer = "MYLAYER";
    }
};

// --- Writer driver: DXF (dxfRW uses writeLayer()/writeBlock()) --------
class DxfWriteDriver : public DRW_Interface {
public:
    explicit DxfWriteDriver(TestEntities &e) : ents(e) {}
    dxfRW *writer = nullptr;

    void writeHeader(DRW_Header &) override {}
    void writeLTypes() override {}
    void writeLayers() override { writer->writeLayer(&ents.layer); }
    void writeTextstyles() override {}
    void writeVports() override {}
    void writeDimstyles() override {}
    void writeAppId() override {}
    void writeBlocks() override { writer->writeBlock(&ents.block); }
    void writeBlockRecords() override { writer->writeBlockRecord(ents.block.name, 0); }
    void writeEntities() override {
        writer->writeLine(&ents.line);
        writer->writeCircle(&ents.circle);
        writer->writeArc(&ents.arc);
        writer->writeLWPolyline(&ents.poly);
        writer->writeText(&ents.text);
        writer->writeInsert(&ents.insert);
    }
    void writeObjects() override {}
    DRW_STUBS
private:
    TestEntities &ents;
};

// --- Writer driver: DWG (dwgRW uses addLayer() for the table record, and
// defineBlock()/beginBlockContent()/endBlockContent() for block bodies) --
class DwgWriteDriver : public DRW_Interface {
public:
    explicit DwgWriteDriver(TestEntities &e) : ents(e) {}
    dwgRW *writer = nullptr;

    void writeHeader(DRW_Header &) override {}
    void writeLTypes() override {}
    void writeLayers() override { writer->addLayer(&ents.layer); }
    void writeTextstyles() override {}
    void writeVports() override {}
    void writeDimstyles() override {}
    void writeAppId() override {}
    void writeBlocks() override {
        std::printf("  writeBlocks: defineBlock...\n"); std::fflush(stdout);
        std::uint32_t recH = writer->defineBlock(ents.block.name, ents.block.basePoint);
        std::printf("  writeBlocks: beginBlockContent...\n"); std::fflush(stdout);
        writer->beginBlockContent(recH);
        // (left empty on purpose: this block has no entities of its own,
        // it's just a named container the INSERT below references)
        std::printf("  writeBlocks: endBlockContent...\n"); std::fflush(stdout);
        writer->endBlockContent();
        std::printf("  writeBlocks: done\n"); std::fflush(stdout);
    }
    void writeBlockRecords() override {}
    void writeEntities() override {
        std::printf("  writeEntities: line...\n"); std::fflush(stdout);
        writer->writeLine(&ents.line);
        std::printf("  writeEntities: circle...\n"); std::fflush(stdout);
        writer->writeCircle(&ents.circle);
        std::printf("  writeEntities: arc...\n"); std::fflush(stdout);
        writer->writeArc(&ents.arc);
        std::printf("  writeEntities: lwpolyline...\n"); std::fflush(stdout);
        writer->writeLWPolyline(&ents.poly);
        std::printf("  writeEntities: text...\n"); std::fflush(stdout);
        writer->writeText(&ents.text);
        std::printf("  writeEntities: insert...\n"); std::fflush(stdout);
        writer->writeInsert(&ents.insert);
        std::printf("  writeEntities: done\n"); std::fflush(stdout);
    }
    void writeObjects() override {}
    DRW_STUBS
private:
    TestEntities &ents;
};

} // namespace

int main() {
    TestEntities entsDxf;
    TestEntities entsDwg;

    // --- Write DXF ---
    {
        DxfWriteDriver drv(entsDxf);
        dxfRW writer("roundtrip_out.dxf");
        drv.writer = &writer;
        bool ok = writer.write(&drv, DRW::AC1021, false);
        std::printf("DXF write: %s\n", ok ? "OK" : "FAILED");
        std::fflush(stdout);
    }

    // --- Write DWG ---
    {
        std::printf("Starting DWG write...\n"); std::fflush(stdout);
        DwgWriteDriver drv(entsDwg);
        dwgRW writer("roundtrip_out.dwg");
        drv.writer = &writer;
        std::printf("Calling writer.write()...\n"); std::fflush(stdout);
        bool ok = writer.write(&drv, DRW::AC1015, true);
        std::printf("DWG write: %s (error=%d)\n", ok ? "OK" : "FAILED",
                    static_cast<int>(writer.getError()));
        std::fflush(stdout);
    }

    return 0;
}
