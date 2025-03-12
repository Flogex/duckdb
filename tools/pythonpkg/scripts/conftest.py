import pathlib
import pytest
import random
import typing
import warnings
import skipped_tests

SQLLOGIC_TEST_CASE_NAME = "test_sqllogic"
SQLLOGIC_TEST_PARAMETER = "test_script_path"


def pytest_addoption(parser: pytest.Parser):
    parser.addoption(
        "--tests-dir",
        required=True,
        type=pathlib.Path,
        dest="duckdb_tests_dir",
        help="Path to the DuckDB tests directory",
    )
    parser.addoption("--start-offset", type=int, dest="start_offset", help="Index of the first test to run")
    parser.addoption("--end-offset", type=int, dest="end_offset", help="Index of the last test to run")
    parser.addoption("--order", choices=["decl", "lex", "rand"], default="decl", dest="order", help="Order of tests")
    parser.addoption("--rng-seed", type=int, dest="rng_seed", help="Random integer seed")


@pytest.hookimpl(tryfirst=True)
def pytest_keyboard_interrupt(excinfo: pytest.ExceptionInfo):
    # TODO: CTRL+C does not immediately interrupt pytest. You sometimes have to press it multiple times.
    pytestmark = pytest.mark.skip(reason="Keyboard interrupt")


def pytest_configure(config: pytest.Config):
    # TODO: Change working directory to temp file. Store original working directory.
    # See https://stackoverflow.com/a/62055409/8336143
    # duckdb::SetTestDirectory can probably be replaced with entry in pytest config
    # temp_dir = pathlib.Path(tempfile.gettempdir()) / "duckdb_unittest_tempdir"

    rng_seed = config.getoption("rng_seed")
    if rng_seed is not None:
        random.seed(rng_seed)


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
    # TODO: Do we have to handle --ignore and --ignore-glob and --deselect

    assert root_dir.is_dir() and root_dir.name == "test"
    assert root_dir.parent.name == "duckdb"

    # Test IDs are the path of the script starting from the test/ directory.
    get_id = lambda path: str(path.relative_to(root_dir.parent))

    # Tests are tagged with the their category (i.e., name of their parent directory)
    known_categories = set()

    def get_tags(path):
        category = path.parent.name
        if category not in known_categories:
            config.addinivalue_line("markers", category)
            known_categories.add(category)

        test_name = get_id(path)
        marks = [pytest.mark.__getattr__(category)]
        if test_name in skipped_tests.FAILING_TESTS:
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

    # Order tests based on --order option. Take as is if order is "decl".
    if config.getoption("order") == "rand":
        random.shuffle(items)
    elif config.getoption("order") == "lex":
        items.sort(key=lambda item: item.name)


    for index, item in enumerate(items):
        # Store some information that are later used in pytest_runtest_logreport.
        # We store the test index after sorting but before deselecting to match start and end offset.
        item.user_properties.append(("test_index", index))
        item.user_properties.append(("total_num_tests", len(items)))
        item.user_properties.append(("report_verbosity", config.get_verbosity()))

    deselected_items = items[:start_offset] + items[end_offset + 1 :]
    config.hook.pytest_deselected(items=deselected_items)
    items[:] = items[start_offset : end_offset + 1]


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_logreport(report: pytest.TestReport):
    """
    Show the test index after the test name
    """

    def get_from_tuple_list(tuples, key):
        for item in tuples:
            if item[0] == key:
                return item[1]
        return None

    if report.when == 'call' and get_from_tuple_list(report.user_properties, "report_verbosity") > 0:
        idx = get_from_tuple_list(report.user_properties, "test_index")
        # index is 0-based, but total_num_tests 1-based
        max_idx = get_from_tuple_list(report.user_properties, "total_num_tests") - 1
        print(f"[{idx}/{max_idx}]", end=" ")
