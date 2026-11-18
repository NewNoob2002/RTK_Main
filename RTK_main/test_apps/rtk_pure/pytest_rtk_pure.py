# 仅定义上板入口；执行需要另行授权烧录与串口访问。
import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize

@pytest.mark.generic
@idf_parametrize('target', ['esp32', 'esp32c3'], indirect=['target'])
def test_rtk_pure(dut: Dut) -> None:
    """等待自动执行的 Unity 汇总；由 pytest-embedded 检查失败项。"""
    dut.expect_unity_test_output(timeout=60)
