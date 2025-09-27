const { Logger } = require('../common.js');

describe('Logger', () => {
  const fixedTimestamp = new Date('2020-04-05T06:07:08.009Z').getTime();
  let dateSpy;
  let consoleSpy;

  const expectedTimeString = () => {
    const now = new Date(fixedTimestamp);
    const year = now.getFullYear();
    const month = now.getMonth() + 1;
    const day = now.getDate();
    const hour = now.getHours();
    const min = now.getMinutes();
    const sec = now.getSeconds();
    const ms = now.getMilliseconds();
    return `${year}-${month}-${day} ${hour}:${min}:${sec}:${ms}`;
  };

  beforeEach(() => {
    dateSpy = jest.spyOn(Date, 'now').mockReturnValue(fixedTimestamp);
    consoleSpy = jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    dateSpy.mockRestore();
    consoleSpy.mockRestore();
  });

  test('currentTimeStr returns a formatted timestamp', () => {
    const logger = new Logger('UnitTest');
    expect(logger.currentTimeStr()).toBe(expectedTimeString());
  });

  test('logInfo prefixes messages with module and info flag', () => {
    const logger = new Logger('UnitTest');
    logger.logInfo('ready');
    expect(consoleSpy).toHaveBeenCalledWith(`[${expectedTimeString()}][UnitTest][IF] ready`);
  });

  test('logError prefixes messages with module and error flag', () => {
    const logger = new Logger('UnitTest');
    logger.logError('fail');
    expect(consoleSpy).toHaveBeenCalledWith(`[${expectedTimeString()}][UnitTest][ER] fail`);
  });
});
