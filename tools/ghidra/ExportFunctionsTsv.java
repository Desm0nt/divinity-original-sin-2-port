// [NR-16] Ghidra script: every function of the open program as "<offset from the image base, hex>\t<name>" for tools/dos2_perf_map.py
// (--funcs main=main_funcs.tsv). Run it on our own headless copy of the project (never the shared GUI project, which another session may
// hold open), e.g.:
//   <ghidra>/support/analyzeHeadless <project dir> <project name> -process main.raw -noanalysis -readOnly
//       -scriptPath <tree>/tools/ghidra -postScript ExportFunctionsTsv.java <out>/main_funcs.tsv
// The offsets are from the program's image base, i.e. the "main+0x..." of the port's logs when main.raw was imported at its module
// layout (base 0 or the module base - both give the same offsets).
//@category Export
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.File;
import java.io.PrintWriter;

public class ExportFunctionsTsv extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : "functions.tsv");
        long base = currentProgram.getImageBase().getOffset();
        int n = 0;
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("# offset\tname (" + currentProgram.getName() + ", image base 0x" + Long.toHexString(base) + ")");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                w.println("0x" + Long.toHexString(f.getEntryPoint().getOffset() - base) + "\t" + f.getName(true));
                n++;
            }
        }
        println("ExportFunctionsTsv: " + n + " functions -> " + out.getAbsolutePath());
    }
}
