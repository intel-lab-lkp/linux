from collections import namedtuple
import glob
import os
import subprocess
from tempfile import NamedTemporaryFile
import unittest


LINUX_ROOT = os.path.abspath(os.path.join(__file__, "..", "..", ".."))
ENV_WITH_DTC = {
    "PATH": os.path.join(LINUX_ROOT, "scripts", "dtc") + os.pathsep + os.environ["PATH"]
}


TestCase = namedtuple("TestCase", ["score_all", "board_id", "output"])

test_cases = [
    TestCase(
        # A board_id that could be provided by firmware
        board_id="""
        qcom,soc = <QCOM_ID_SM8650>;
        qcom,soc-version = <QCOM_ID_SM8650 QCOM_SOC_REVISION(1)>;
        qcom,platform = <QCOM_BOARD_ID_MTP>;
        qcom,platform-type = <QCOM_BOARD_ID_MTP 0>;
        qcom,platform-version = <QCOM_BOARD_ID_MTP 0 0x0100>;
        qcom,boot-device = <QCOM_BOARD_BOOT_UFS>;
        """,
        score_all=False,
        output="""
        qcom/sm8650-mtp.dtb
        """,
    ),
    TestCase(
        # A board_id that could be provided by firmware
        board_id="""
        qcom,soc = <QCOM_ID_SM8550>;
        qcom,soc-version = <QCOM_ID_SM8550 QCOM_SOC_REVISION(1)>;
        qcom,platform = <QCOM_BOARD_ID_MTP>;
        qcom,platform-type = <QCOM_BOARD_ID_MTP 0>;
        qcom,platform-version = <QCOM_BOARD_ID_MTP 0 0x0100>;
        qcom,boot-device = <QCOM_BOARD_BOOT_UFS>;
        """,
        score_all=True,
        output="""
        qcom/sm8550.dtb: 1
        qcom/sm8550-mtp.dtb: 3
        qcom/sm8550-mtp.dtbo: 2
        """,
    ),
]


def compile_board_id(board_id: str):
    dts = f"""
        /dts-v1/;

        #include <dt-bindings/arm/qcom,ids.h>

        / {{
            compatible = "linux,dummy";
            board-id {{
                {board_id}
            }};
        }};
        """
    dts_processed = subprocess.run(
        [
            "gcc",
            "-E",
            "-nostdinc",
            f"-I{os.path.join(LINUX_ROOT, 'scripts', 'dtc', 'include-prefixes')}",
            "-undef",
            "-D__DTS__",
            "-x",
            "assembler-with-cpp",
            "-o" "-",
            "-",
        ],
        stdout=subprocess.PIPE,
        input=dts.encode("utf-8"),
        check=True,
    )
    dtc = subprocess.run(
        ["dtc", "-I", "dts", "-O", "dtb", "-o", "-", "-"],
        stdout=subprocess.PIPE,
        input=dts_processed.stdout,
        env=ENV_WITH_DTC,
    )
    return dtc.stdout


def select_boards(score_all, fwdtb):
    with NamedTemporaryFile() as fwdtb_file:
        fwdtb_file.write(fwdtb)
        fwdtb_file.flush()
        root_dir = os.path.join(LINUX_ROOT, "arch", "arm64", "boot", "dts")
        return subprocess.run(
            filter(
                bool,
                [
                    "fdt-select-board",
                    "-a" if score_all else None,
                    "-r",
                    fwdtb_file.name,
                    *glob.glob(
                        "qcom/*.dtb*",
                        root_dir=root_dir,
                    ),
                ],
            ),
            stdout=subprocess.PIPE,
            text=True,
            cwd=root_dir,
            env=ENV_WITH_DTC,
            stderr=subprocess.STDOUT,
        )


def fixup_lines(s):
    return '\n'.join(filter(bool, sorted(_s.strip() for _s in s.split('\n'))))


class TestBoardIds(unittest.TestCase):
    def __init__(self, index: int, args: TestCase) -> None:
        super().__init__()
        self.args = args
        self.index = index

    def runTest(self):
        fwdtb = compile_board_id(self.args.board_id)
        output = select_boards(self.args.score_all, fwdtb)
        if output.stderr:
            self.assertMultiLineEqual(output.stderr, "")
        expected = fixup_lines(self.args.output)
        actual = fixup_lines(output.stdout)
        self.assertMultiLineEqual(expected, actual)

    def __str__(self):
        return f"Test case {self.index}"


def suite():
    suite = unittest.TestSuite()
    for idx, test in enumerate(test_cases):
        suite.addTest(TestBoardIds(idx + 1, test))
    return suite


if __name__ == "__main__":
    runner = unittest.TextTestRunner()
    runner.run(suite())
