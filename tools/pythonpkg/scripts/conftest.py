import pathlib
import pytest
import typing
import warnings
import skipped_tests

SQLLOGIC_TEST_CASE_NAME = "test_sqllogic"
SQLLOGIC_TEST_PARAMETER = "test_script_path"

def pytest_addoption(parser: pytest.Parser):
    parser.addoption(
        "--tests-dir", required=True, type=pathlib.Path, dest="duckdb_tests_dir", help="Path to the DuckDB tests directory"
    )
    parser.addoption(
        "--start-offset", type=int, dest="start_offset", help="Index of the first test to run"
    )
    parser.addoption(
        "--end-offset", type=int, dest="end_offset", help="Index of the last test to run"
    )

def pytest_keyboard_interrupt(excinfo: pytest.ExceptionInfo):
    #TODO: CTRL+C interrupts DuckDB queries and does not go immediately to pytest
    pytestmark = pytest.mark.skip(reason="Keyboard interrupt")

def pytest_configure(config: pytest.Config):
    # TODO: Change working directory to temp file. Store original working directory.
    # See https://stackoverflow.com/a/62055409/8336143
    # duckdb::SetTestDirectory can probably be replaced with entry in pytest config
    # temp_dir = pathlib.Path(tempfile.gettempdir()) / "duckdb_unittest_tempdir"
    pass


def pytest_unconfigure(config: pytest.Config):
    # TODO: Restore original working directory and delete temp directory
    pass


def scan_for_test_scripts(root_dir: pathlib.Path, config: pytest.Config) -> typing.Iterator[typing.Any]:
    """
    Scans for .test files in the given directory and its subdirectories.
    Returns an iterator of pytest parameters (argument, id and marks).
    """

    # TODO: Add tests from extensions
    # TODO: Add .test_slow and .test_coverage files

    assert root_dir.is_dir() and root_dir.name == "test"
    assert root_dir.parent.name == "duckdb"

    # Test IDs are the path of the script starting from the test/ directory.
    get_id = lambda path: str(path.relative_to(root_dir.parent))

    # Tests are tagged with the name of their parent directory
    def get_tags(path):
        # TODO: Do this only once per marker
        config.addinivalue_line(
            "markers", path.parent.name
        )
        test_name = get_id(path)
        marks = [pytest.mark.__getattr__(path.parent.name)]
        if test_name in skipped_tests.failing_tests:
            marks.append(pytest.mark.xfail)
        if test_name in skipped_tests.SKIPPED_TESTS:
            marks.append(pytest.mark.skip)
        return marks

    it = root_dir.rglob("*.test")
    return map(lambda path: pytest.param(path.absolute(), id=get_id(path), marks=get_tags(path)), it)


def pytest_generate_tests(metafunc: pytest.Metafunc):
    # test_sqllogic (a.k.a SQLLOGIC_TEST_CASE_NAME) is defined in test_sqllogic.py
    if metafunc.definition.name == SQLLOGIC_TEST_CASE_NAME:
        tests_dir: pathlib.Path = metafunc.config.getoption("duckdb_tests_dir")
        assert tests_dir is not None
        # Create absolute & normalized path
        tests_dir = tests_dir.resolve()

        metafunc.parametrize(
            SQLLOGIC_TEST_PARAMETER,
            scan_for_test_scripts(tests_dir, metafunc.config),
        )

def pytest_collection_modifyitems(session: pytest.Session, config: pytest.Config, items: list[pytest.Item]):
    if len(items) == 0:
        warnings.warn("No tests were found. Check that you passed the correct directory via --tests-dir.")
        return

    start_offset = config.getoption("start_offset")
    if start_offset is None:
        start_offset = 0

    end_offset = config.getoption("end_offset")
    max_end_offset = len(items) - 1
    if end_offset is None or end_offset > max_end_offset:
        end_offset = max_end_offset

    if start_offset < 0:
        raise ValueError("--start-offset must be a non-negative integer")
    elif end_offset < start_offset:
        raise ValueError("--end-offset must be greater than or equal to --start-offset")
    
    items.sort(key=lambda item: item.name)

    deselected_items = items[:start_offset] + items[end_offset + 1:]
    config.hook.pytest_deselected(items=deselected_items)
    items[:] = items[start_offset:end_offset + 1]
    